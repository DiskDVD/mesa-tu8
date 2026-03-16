/*
 * Copyright © 2021 Igalia S.L.
 * SPDX-License-Identifier: MIT
 *
 * ОПТИМИЗАЦИЯ ДЛЯ ADRENO 810: GMEM 512KB, МАКСИМУМ FPS
 */

#include "tu_autotune.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_pass.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

void
tu_autotune_free_results_locked(struct tu_device *dev, struct list_head *results);

#define TU_AUTOTUNE_DEBUG_LOG 0

/* How many last renderpass stats are taken into account. */
#define MAX_HISTORY_RESULTS 5
/* For how many submissions we store renderpass stats. */
#define MAX_HISTORY_LIFETIME 128

/* Для A810 - форсируем GMEM всегда */
#define A810_FORCE_GMEM true

/**
 * Tracks results for a given renderpass key
 */
struct tu_renderpass_history {
   uint64_t key;

   /* We would delete old history entries */
   uint32_t last_fence;

   /**
    * List of recent fd_renderpass_result's
    */
   struct list_head results;
   uint32_t num_results;

   uint32_t avg_samples;
};

/* Holds per-submission cs which writes the fence. */
struct tu_submission_data {
   struct list_head node;
   uint32_t fence;

   struct tu_cs fence_cs;
};

static bool
fence_before(uint32_t a, uint32_t b)
{
   return (int32_t)(a - b) < 0;
}

static uint32_t
get_autotune_fence(struct tu_autotune *at)
{
   return at->device->global_bo_map->autotune_fence;
}

template <chip CHIP>
static void
create_submission_fence(struct tu_device *dev,
                        struct tu_cs *cs,
                        uint32_t fence)
{
   uint64_t dst_iova = dev->global_bo->iova + gb_offset(autotune_fence);
   if (CHIP >= A7XX) {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 4);
      tu_cs_emit(cs,
         CP_EVENT_WRITE7_0(.event = CACHE_FLUSH_TS,
                           .write_src = EV_WRITE_USER_32B,
                           .write_dst = EV_DST_RAM,
                           .write_enabled = true).value);
   } else {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
      tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   }

   tu_cs_emit_qw(cs, dst_iova);
   tu_cs_emit(cs, fence);
}

static struct tu_submission_data *
create_submission_data(struct tu_device *dev, struct tu_autotune *at,
                       uint32_t fence)
{
   struct tu_submission_data *submission_data = NULL;
   if (!list_is_empty(&at->submission_data_pool)) {
      submission_data = list_first_entry(&at->submission_data_pool,
                                         struct tu_submission_data, node);
      list_del(&submission_data->node);
   } else {
      submission_data = (struct tu_submission_data *) calloc(
         1, sizeof(struct tu_submission_data));
      tu_cs_init(&submission_data->fence_cs, dev, TU_CS_MODE_GROW, 5, "autotune fence cs");
   }
   submission_data->fence = fence;

   struct tu_cs* fence_cs = &submission_data->fence_cs;
   tu_cs_begin(fence_cs);
   TU_CALLX(dev, create_submission_fence)(dev, fence_cs, fence);
   tu_cs_end(fence_cs);

   list_addtail(&submission_data->node, &at->pending_submission_data);

   return submission_data;
}

static void
finish_submission_data(struct tu_autotune *at,
                       struct tu_submission_data *data)
{
   list_del(&data->node);
   list_addtail(&data->node, &at->submission_data_pool);
   tu_cs_reset(&data->fence_cs);
}

static void
free_submission_data(struct tu_submission_data *data)
{
   list_del(&data->node);
   tu_cs_finish(&data->fence_cs);

   free(data);
}

static uint64_t
hash_renderpass_instance(const struct tu_render_pass *pass,
                         const struct tu_framebuffer *framebuffer,
                         const struct tu_cmd_buffer *cmd) {
   uint32_t data[3 + pass->attachment_count * 5];
   uint32_t* ptr = data;

   *ptr++ = framebuffer->width;
   *ptr++ = framebuffer->height;
   *ptr++ = framebuffer->layers;

   for (unsigned i = 0; i < pass->attachment_count; i++) {
      *ptr++ = cmd->state.attachments[i]->view.width;
      *ptr++ = cmd->state.attachments[i]->view.height;
      *ptr++ = cmd->state.attachments[i]->image->vk.format;
      *ptr++ = cmd->state.attachments[i]->image->vk.array_layers;
      *ptr++ = cmd->state.attachments[i]->image->vk.mip_levels;
   }

   return XXH64(data, sizeof(data), pass->autotune_hash);
}

static void
free_result(struct tu_device *dev, struct tu_renderpass_result *result)
{
   tu_suballoc_bo_free(&dev->autotune_suballoc, &result->bo);
   list_del(&result->node);
   free(result);
}

static void
free_history(struct tu_device *dev, struct tu_renderpass_history *history)
{
   tu_autotune_free_results_locked(dev, &history->results);
   free(history);
}

static bool
get_history(struct tu_autotune *at, uint64_t rp_key, uint32_t *avg_samples)
{
   bool has_history = false;

   u_rwlock_rdlock(&at->ht_lock);
   struct hash_entry *entry =
      _mesa_hash_table_search(at->ht, &rp_key);
   if (entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      if (history->num_results > 0) {
         *avg_samples = p_atomic_read(&history->avg_samples);
         has_history = true;
      }
   }
   u_rwlock_rdunlock(&at->ht_lock);

   return has_history;
}

static struct tu_renderpass_result *
create_history_result(struct tu_autotune *at, uint64_t rp_key)
{
   struct tu_renderpass_result *result =
      (struct tu_renderpass_result *) calloc(1, sizeof(*result));
   result->rp_key = rp_key;

   return result;
}

static void
history_add_result(struct tu_device *dev, struct tu_renderpass_history *history,
                      struct tu_renderpass_result *result)
{
   uint32_t max_results = MAX_HISTORY_RESULTS;

   list_delinit(&result->node);
   list_add(&result->node, &history->results);

   if (history->num_results < max_results) {
      history->num_results++;
   } else {
      struct tu_renderpass_result *old_result =
         list_last_entry(&history->results, struct tu_renderpass_result, node);
      mtx_lock(&dev->autotune_mutex);
      free_result(dev, old_result);
      mtx_unlock(&dev->autotune_mutex);
   }

   uint32_t total_samples = 0;
   list_for_each_entry(struct tu_renderpass_result, result,
                       &history->results, node) {
      total_samples += result->samples_passed;
   }

   float avg_samples = (float)total_samples / (float)history->num_results;
   p_atomic_set(&history->avg_samples, (uint32_t)avg_samples);
}

static void
process_results(struct tu_autotune *at, uint32_t current_fence)
{
   struct tu_device *dev = at->device;

   list_for_each_entry_safe(struct tu_renderpass_result, result,
                            &at->pending_results, node) {
      if (fence_before(current_fence, result->fence))
         break;

      struct tu_renderpass_history *history = result->history;
      result->samples_passed =
         result->samples->samples_end - result->samples->samples_start;

      history_add_result(dev, history, result);
   }

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->pending_submission_data, node) {
      if (fence_before(current_fence, submission_data->fence))
         break;

      finish_submission_data(at, submission_data);
   }
}

static void
queue_pending_results(struct tu_autotune *at, struct tu_cmd_buffer *cmdbuf)
{
   bool one_time_submit = cmdbuf->usage_flags &
         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

   if (one_time_submit) {
      list_splicetail(&cmdbuf->renderpass_autotune_results,
                        &at->pending_results);
      list_inithead(&cmdbuf->renderpass_autotune_results);
   } else {
      list_for_each_entry_safe(struct tu_renderpass_result, result,
                              &cmdbuf->renderpass_autotune_results, node) {
         struct tu_renderpass_result *copy =
            (struct tu_renderpass_result *) malloc(sizeof(*result));
         *copy = *result;
         tu_bo_get_ref(copy->bo.bo);
         list_addtail(&copy->node, &at->pending_results);
      }
   }
}

struct tu_cs *
tu_autotune_on_submit(struct tu_device *dev,
                      struct tu_autotune *at,
                      struct tu_cmd_buffer **cmd_buffers,
                      uint32_t cmd_buffer_count)
{
   const uint32_t gpu_fence = get_autotune_fence(at);
   const uint32_t new_fence = at->fence_counter++;

   process_results(at, gpu_fence);

   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      list_for_each_entry_safe(struct tu_renderpass_result, result,
                          &cmdbuf->renderpass_autotune_results, node) {
         struct tu_renderpass_history *history;
         struct hash_entry *entry =
            _mesa_hash_table_search(at->ht, &result->rp_key);
         if (!entry) {
            history =
               (struct tu_renderpass_history *) calloc(1, sizeof(*history));
            history->key = result->rp_key;
            list_inithead(&history->results);

            u_rwlock_wrlock(&at->ht_lock);
            _mesa_hash_table_insert(at->ht, &history->key, history);
            u_rwlock_wrunlock(&at->ht_lock);
         } else {
            history = (struct tu_renderpass_history *) entry->data;
         }

         history->last_fence = new_fence;

         result->fence = new_fence;
         result->history = history;
      }
   }

   struct tu_submission_data *submission_data =
      create_submission_data(dev, at, new_fence);

   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      if (list_is_empty(&cmdbuf->renderpass_autotune_results))
         continue;

      queue_pending_results(at, cmdbuf);
   }

   if (TU_AUTOTUNE_DEBUG_LOG)
      mesa_logi("Total history entries: %u", at->ht->entries);

   uint32_t history_lifetime = MAX_HISTORY_LIFETIME;

   hash_table_foreach(at->ht, entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      if (fence_before(gpu_fence, history->last_fence + history_lifetime))
         continue;

      if (TU_AUTOTUNE_DEBUG_LOG)
         mesa_logi("Removed old history entry %016" PRIx64 "", history->key);

      u_rwlock_wrlock(&at->ht_lock);
      _mesa_hash_table_remove_key(at->ht, &history->key);
      u_rwlock_wrunlock(&at->ht_lock);

      mtx_lock(&dev->autotune_mutex);
      free_history(dev, history);
      mtx_unlock(&dev->autotune_mutex);
   }

   return &submission_data->fence_cs;
}

static bool
renderpass_key_equals(const void *_a, const void *_b)
{
   return *(uint64_t *)_a == *(uint64_t *)_b;
}

static uint32_t
renderpass_key_hash(const void *_a)
{
   return *((uint64_t *) _a) & 0xffffffff;
}

VkResult
tu_autotune_init(struct tu_autotune *at, struct tu_device *dev)
{
   at->enabled = true;
   at->device = dev;
   at->ht = _mesa_hash_table_create(NULL,
                                    renderpass_key_hash,
                                    renderpass_key_equals);
   u_rwlock_init(&at->ht_lock);

   list_inithead(&at->pending_results);
   list_inithead(&at->pending_submission_data);
   list_inithead(&at->submission_data_pool);

   at->fence_counter = 1;

   return VK_SUCCESS;
}

void
tu_autotune_fini(struct tu_autotune *at, struct tu_device *dev)
{
   if (TU_AUTOTUNE_LOG_AT_FINISH) {
      while (!list_is_empty(&at->pending_results)) {
         const uint32_t gpu_fence = get_autotune_fence(at);
         process_results(at, gpu_fence);
      }

      hash_table_foreach(at->ht, entry) {
         struct tu_renderpass_history *history =
            (struct tu_renderpass_history *) entry->data;

         mesa_logi("%016" PRIx64 " \tavg_passed=%u results=%u",
                   history->key, history->avg_samples, history->num_results);
      }
   }

   tu_autotune_free_results(dev, &at->pending_results);

   mtx_lock(&dev->autotune_mutex);
   hash_table_foreach(at->ht, entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      free_history(dev, history);
   }
   mtx_unlock(&dev->autotune_mutex);

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->pending_submission_data, node) {
      free_submission_data(submission_data);
   }

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->submission_data_pool, node) {
      free_submission_data(submission_data);
   }

   _mesa_hash_table_destroy(at->ht, NULL);
   u_rwlock_destroy(&at->ht_lock);
}

bool
tu_autotune_submit_requires_fence(struct tu_cmd_buffer **cmd_buffers,
                                  uint32_t cmd_buffer_count)
{
   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      if (!list_is_empty(&cmdbuf->renderpass_autotune_results))
         return true;
   }

   return false;
}

void
tu_autotune_free_results_locked(struct tu_device *dev, struct list_head *results)
{
   list_for_each_entry_safe(struct tu_renderpass_result, result,
                            results, node) {
      free_result(dev, result);
   }
}

void
tu_autotune_free_results(struct tu_device *dev, struct list_head *results)
{
   mtx_lock(&dev->autotune_mutex);
   tu_autotune_free_results_locked(dev, results);
   mtx_unlock(&dev->autotune_mutex);
}

bool
tu_autotune_use_bypass(struct tu_autotune *at,
                       struct tu_cmd_buffer *cmd_buffer,
                       struct tu_renderpass_result **autotune_result)
{
   /* ===== ДЛЯ ADRENO 810: ФОРСИРУЕМ GMEM ===== */
   if (cmd_buffer->device->physical_device->dev_id.gpu_id == 810) {
      /* Всегда используем GMEM для максимального FPS */
      return false;
   }
   /* ===== КОНЕЦ ОПТИМИЗАЦИИ ===== */

   /* Для остальных GPU - стандартная логика */
   const struct tu_render_pass *pass = cmd_buffer->state.pass;
   const struct tu_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   if (cmd_buffer->state.rp.sysmem_single_prim_mode)
      return false;

   if (pass->has_fdm)
      return false;

   bool simultaneous_use =
      cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

   if (!at->enabled || simultaneous_use) {
      for (unsigned i = 0; i < pass->subpass_count; i++) {
         if (pass->subpasses[i].samples != VK_SAMPLE_COUNT_1_BIT)
            return false;
      }
      if (cmd_buffer->state.rp.drawcall_count > 5)
         return false;
      return true;
   }

   uint64_t renderpass_key = hash_renderpass_instance(pass, framebuffer, cmd_buffer);

   *autotune_result = create_history_result(at, renderpass_key);

   uint32_t avg_samples = 0;
   if (get_history(at, renderpass_key, &avg_samples)) {
      uint32_t pass_pixel_count;
      if (cmd_buffer->state.per_layer_render_area) {
         pass_pixel_count = 0;
         for (unsigned i = 0; i < cmd_buffer->state.pass->num_views; i++) {
            const VkExtent2D *extent = &cmd_buffer->state.render_areas[i].extent;
            pass_pixel_count += extent->width * extent->height;
         }
      } else {
         const VkExtent2D *extent = &cmd_buffer->state.render_areas[0].extent;
         pass_pixel_count = extent->width * extent->height *
            MAX2(cmd_buffer->state.pass->num_views, cmd_buffer->state.framebuffer->layers);
      }

      uint64_t sysmem_bandwidth =
         (uint64_t)pass->sysmem_bandwidth_per_pixel * pass_pixel_count;
      uint64_t gmem_bandwidth =
         (uint64_t)pass->gmem_bandwidth_per_pixel * pass_pixel_count;

      uint64_t total_draw_call_bandwidth = 0;
      if (cmd_buffer->state.rp.drawcall_count) {
         total_draw_call_bandwidth =
            (uint64_t)avg_samples *
            cmd_buffer->state.rp.drawcall_bandwidth_per_sample_sum /
            cmd_buffer->state.rp.drawcall_count;
      }

      sysmem_bandwidth += total_draw_call_bandwidth;
      gmem_bandwidth = (gmem_bandwidth * 11 + total_draw_call_bandwidth) / 10;

      return sysmem_bandwidth <= gmem_bandwidth;
   }

   for (unsigned i = 0; i < pass->subpass_count; i++) {
      if (pass->subpasses[i].samples != VK_SAMPLE_COUNT_1_BIT)
         return false;
   }
   if (cmd_buffer->state.rp.drawcall_count > 5)
      return false;
   return true;
}

template <chip CHIP>
void
tu_autotune_begin_renderpass(struct tu_cmd_buffer *cmd,
                             struct tu_cs *cs,
                             struct tu_renderpass_result *autotune_result)
{
   if (!autotune_result)
      return;

   struct tu_device *dev = cmd->device;

   static const uint32_t size = sizeof(struct tu_renderpass_samples);

   mtx_lock(&dev->autotune_mutex);
   VkResult ret = tu_suballoc_bo_alloc(&autotune_result->bo, &dev->autotune_suballoc, size, size);
   mtx_unlock(&dev->autotune_mutex);
   if (ret != VK_SUCCESS) {
      autotune_result->bo.iova = 0;
      return;
   }

   uint64_t result_iova = autotune_result->bo.iova;

   autotune_result->samples =
      (struct tu_renderpass_samples *) tu_suballoc_bo_map(
         &autotune_result->bo);

   tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));
   if (cmd->device->physical_device->info->props.has_event_write_sample_count) {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
      tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                       .write_sample_count = true).value);
      tu_cs_emit_qw(cs, result_iova);

      if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                          .write_sample_count = true,
                                          .sample_count_end_offset = true,
                                          .write_accum_sample_count_diff = true).value);
         tu_cs_emit_qw(cs, result_iova);
      }
   } else {
      tu_cs_emit_regs(cs,
                        A6XX_RB_SAMPLE_COUNTER_BASE(.qword = result_iova));
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
      tu_cs_emit(cs, ZPASS_DONE);
   }
}
TU_GENX(tu_autotune_begin_renderpass);

template <chip CHIP>
void tu_autotune_end_renderpass(struct tu_cmd_buffer *cmd,
                                struct tu_cs *cs,
                                struct tu_renderpass_result *autotune_result)
{
   if (!autotune_result)
      return;

   if (!autotune_result->bo.iova)
      return;

   uint64_t result_iova = autotune_result->bo.iova;

   tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));

   if (cmd->device->physical_device->info->props.has_event_write_sample_count) {
      if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                          .write_sample_count = true).value);
         tu_cs_emit_qw(cs, result_iova + offsetof(struct tu_renderpass_samples, samples_end));
      }

      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
      tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                       .write_sample_count = true,
                                       .sample_count_end_offset = true,
                                       .write_accum_sample_count_diff = true).value);
      tu_cs_emit_qw(cs, result_iova);
   } else {
      result_iova += offsetof(struct tu_renderpass_samples, samples_end);

      tu_cs_emit_regs(cs,
                        A6XX_RB_SAMPLE_COUNTER_BASE(.qword = result_iova));
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
      tu_cs_emit(cs, ZPASS_DONE);
   }
}
TU_GENX(tu_autotune_end_renderpass);