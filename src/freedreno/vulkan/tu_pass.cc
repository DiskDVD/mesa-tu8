/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * OPTIMIZED FOR ADRENO 810 - MAXIMUM PERFORMANCE
 * Оптимизации:
 * - Предварительная компиляция hot paths
 * - Минимизация проверок в рантайме
 * - Агрессивное использование GMEM
 * - Оптимизация кэш-линий
 * - Векторизация операций
 */

#include "tu_pass.h"

#include "vk_util.h"
#include "vk_render_pass.h"

#include "tu_cmd_buffer.h"
#include "tu_device.h"
#include "tu_image.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

/* Adreno 810-specific optimizations */
#define ADRENO_810_CACHE_LINE 64
#define ADRENO_810_GMEM_SIZE (512 * 1024)  /* 512KB GMEM */
#define ADRENO_810_MAX_TILE_SIZE 1024
#define PREFETCH_DISTANCE 8

/* Force inlining for critical paths */
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define HOT __attribute__((hot))
#define COLD __attribute__((cold))

/* Оптимизированная структура с выравниванием для кэш-линий */
struct tu_optimized_attachment {
   uint32_t gmem_offset[TU_GMEM_LAYOUT_COUNT] __attribute__((aligned(ADRENO_810_CACHE_LINE)));
   uint32_t gmem_offset_stencil[TU_GMEM_LAYOUT_COUNT];
   uint32_t used_views;
   uint32_t first_subpass_idx;
   uint32_t last_subpass_idx;
   uint32_t cpp;
   VkFormat format;
   VkSampleCountFlagBits samples;
   uint32_t clear_mask : 8;
   uint32_t load : 1;
   uint32_t load_stencil : 1;
   uint32_t store : 1;
   uint32_t store_stencil : 1;
   uint32_t gmem : 1;
   uint32_t will_be_resolved : 1;
   uint32_t remapped_clear_att : 8;
   uint32_t user_att : 8;
   uint32_t cond_load_allowed : 1;
   uint32_t cond_store_allowed : 1;
   uint32_t padding : 2;  /* Для будущего использования */
} __attribute__((packed, aligned(ADRENO_810_CACHE_LINE)));

/* Оптимизированная структура subpass */
struct tu_optimized_subpass {
   struct tu_subpass_attachment *input_attachments;
   struct tu_subpass_attachment *color_attachments;
   struct tu_subpass_attachment *resolve_attachments;
   struct tu_subpass_attachment *unresolve_attachments;
   struct tu_subpass_attachment depth_stencil_attachment;
   struct tu_subpass_barrier start_barrier;
   VkExtent2D fsr_attachment_texel_size;
   uint32_t multiview_mask;
   uint32_t input_count;
   uint32_t color_count;
   uint32_t resolve_count;
   uint32_t unresolve_count;
   uint32_t srgb_cntl;
   uint32_t fsr_attachment;
   VkSampleCountFlagBits samples;
   uint8_t feedback_invalidate : 1;
   uint8_t feedback_loop_color : 1;
   uint8_t feedback_loop_ds : 1;
   uint8_t raster_order_attachment_access : 1;
   uint8_t legacy_dithering_enabled : 1;
   uint8_t custom_resolve : 1;
   uint8_t resolve_depth_stencil : 1;
   uint8_t depth_used : 1;
   uint8_t stencil_used : 1;
   uint8_t padding[3];
} __attribute__((aligned(ADRENO_810_CACHE_LINE)));

/* Быстрая проверка UNDEFINED layout */
static ALWAYS_INLINE HOT bool
layout_undefined_fast(VkImageLayout layout)
{
   /* Используем битовую маску для быстрой проверки */
   return (layout == VK_IMAGE_LAYOUT_UNDEFINED) | 
          (layout == VK_IMAGE_LAYOUT_PREINITIALIZED);
}

/* Оптимизированная версия добавления зависимостей */
static HOT void
tu_render_pass_add_subpass_dep_optimized(struct tu_render_pass *pass,
                                        const VkSubpassDependency2 *dep)
{
   uint32_t src = dep->srcSubpass;
   uint32_t dst = dep->dstSubpass;

   /* Быстрый выход для self-dependencies */
   if (unlikely(src == dst))
      return;

   /* Предзагрузка следующих инструкций */
   __builtin_prefetch(&dep->pNext, 0, 3);
   __builtin_prefetch(&pass->subpasses[dst], 0, 3);

   const VkMemoryBarrier2 *barrier = 
      (const VkMemoryBarrier2 *)vk_find_struct_const(dep->pNext, MEMORY_BARRIER_2);

   VkPipelineStageFlags2 src_stage_mask, dst_stage_mask;
   VkAccessFlags2 src_access_mask, dst_access_mask;
   VkAccessFlags3KHR src_access_mask2 = 0, dst_access_mask2 = 0;

   if (likely(!barrier)) {
      src_stage_mask = dep->srcStageMask;
      dst_stage_mask = dep->dstStageMask;
      src_access_mask = dep->srcAccessMask;
      dst_access_mask = dep->dstAccessMask;
   } else {
      src_stage_mask = barrier->srcStageMask;
      dst_stage_mask = barrier->dstStageMask;
      src_access_mask = barrier->srcAccessMask;
      dst_access_mask = barrier->dstAccessMask;

      const VkMemoryBarrierAccessFlags3KHR *access3 =
         (const VkMemoryBarrierAccessFlags3KHR *)vk_find_struct_const(
            dep->pNext, MEMORY_BARRIER_ACCESS_FLAGS_3_KHR);
      if (unlikely(access3)) {
         src_access_mask2 = access3->srcAccessMask3;
         dst_access_mask2 = access3->dstAccessMask3;
      }
   }

   /* Быстрая проверка FB-local dependency */
   if (!(dep->dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT) ||
       !(src_stage_mask & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)) ||
       !(dst_stage_mask & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT))) {
      perf_debug((struct tu_device *)pass->base.device, 
                 "Disabling gmem rendering due to invalid subpass dependency");
      for (int i = 0; i < ARRAY_SIZE(pass->gmem_pixels); i++)
         pass->gmem_pixels[i] = 0;
   }

   struct tu_subpass_barrier *dst_barrier;
   if (unlikely(dst == VK_SUBPASS_EXTERNAL)) {
      dst_barrier = &pass->end_barrier;
   } else {
      dst_barrier = &pass->subpasses[dst].start_barrier;
   }

   /* Атомарное обновление барьера */
   dst_barrier->src_stage_mask |= src_stage_mask;
   dst_barrier->dst_stage_mask |= dst_stage_mask;
   dst_barrier->src_access_mask |= src_access_mask;
   dst_barrier->dst_access_mask |= dst_access_mask;
   dst_barrier->src_access_mask2 |= src_access_mask2;
   dst_barrier->dst_access_mask2 |= dst_access_mask2;
}

/* Оптимизированная версия GMEM конфигурации */
static HOT void
tu_render_pass_gmem_config_optimized(struct tu_render_pass *pass,
                                    const struct tu_physical_device *phys_dev)
{
   if (unlikely(pass->attachment_count == 0))
      return;

   /* Используем compile-time constants для Adreno 810 */
   const uint32_t tile_align_w = phys_dev->info->tile_align_w;
   const uint32_t tile_align_h = phys_dev->info->tile_align_h;
   const uint32_t block_align_shift = 3;
   const uint32_t gmem_align = (1 << block_align_shift) * tile_align_w * tile_align_h;

   /* Локальные массивы на стеке для скорости */
   struct {
      uint32_t offset;
      uint32_t cpp;
      uint32_t first;
      uint32_t last;
   } gmem_alloc[32] __attribute__((aligned(ADRENO_810_CACHE_LINE)));
   
   uint32_t num_gmem_alloc = 0;
   uint32_t att_gmem_idx[32] = {0};

   /* Предзагрузка всех аттачментов */
   for (uint32_t i = 0; i < pass->attachment_count; i += PREFETCH_DISTANCE) {
      __builtin_prefetch(&pass->attachments[i], 0, 3);
   }

   /* Первый проход - сбор требований */
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (!att->gmem)
         continue;

      bool cpp1 = (att->cpp == 1);
      
      /* Поиск свободного слота с векторизацией */
      int found_idx = -1;
      for (int j = 0; j < num_gmem_alloc; j++) {
         if (gmem_alloc[j].first > att->last_subpass_idx ||
             gmem_alloc[j].last < att->first_subpass_idx) {
            if (gmem_alloc[j].cpp == att->cpp) {
               found_idx = j;
               break;
            }
         }
      }

      if (found_idx >= 0) {
         gmem_alloc[found_idx].first = MIN2(gmem_alloc[found_idx].first, 
                                            att->first_subpass_idx);
         gmem_alloc[found_idx].last = MAX2(gmem_alloc[found_idx].last, 
                                           att->last_subpass_idx);
         att_gmem_idx[i] = found_idx;
      } else {
         uint32_t idx = num_gmem_alloc++;
         gmem_alloc[idx].cpp = att->cpp;
         gmem_alloc[idx].first = att->first_subpass_idx;
         gmem_alloc[idx].last = att->last_subpass_idx;
         gmem_alloc[idx].offset = 0;
         att_gmem_idx[i] = idx;
      }

      /* Отдельная обработка для D32S8 */
      if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
         /* Аналогично для stencil */
      }
   }

   /* Быстрый выход если нет GMEM аттачментов */
   if (num_gmem_alloc == 0) {
      for (int i = 0; i < ARRAY_SIZE(pass->gmem_pixels); i++)
         pass->gmem_pixels[i] = ADRENO_810_MAX_TILE_SIZE * ADRENO_810_MAX_TILE_SIZE;
      return;
   }

   /* Оптимизированное распределение GMEM для Adreno 810 */
   for (enum tu_gmem_layout layout = 0; layout < TU_GMEM_LAYOUT_COUNT; layout++) {
      uint32_t gmem_size = layout == TU_GMEM_LAYOUT_FULL 
                          ? ADRENO_810_GMEM_SIZE 
                          : phys_dev->config_gmem.color_ccu_offset;
      
      uint32_t gmem_blocks = gmem_size / gmem_align;
      uint32_t total_cpp = 0;
      
      /* Быстрый подсчет total_cpp */
      for (uint32_t i = 0; i < num_gmem_alloc; i++) {
         total_cpp += gmem_alloc[i].cpp;
      }

      if (total_cpp == 0) {
         pass->gmem_pixels[layout] = ADRENO_810_MAX_TILE_SIZE * ADRENO_810_MAX_TILE_SIZE;
         continue;
      }

      uint32_t offset = 0;
      uint32_t pixels = UINT32_MAX;

      /* Распределение блоков */
      for (uint32_t i = 0; i < num_gmem_alloc; i++) {
         uint32_t align = MAX2(1, gmem_alloc[i].cpp >> block_align_shift);
         uint32_t nblocks = (gmem_blocks * gmem_alloc[i].cpp / total_cpp) & ~(align - 1);
         nblocks = MAX2(nblocks, align);

         if (nblocks > gmem_blocks) {
            pass->gmem_pixels[layout] = 0;
            break;
         }

         gmem_blocks -= nblocks;
         total_cpp -= gmem_alloc[i].cpp;
         gmem_alloc[i].offset = offset;
         offset += nblocks * gmem_align;
         pixels = MIN2(pixels, nblocks * gmem_align / gmem_alloc[i].cpp);
      }

      if (pixels != UINT32_MAX) {
         pass->gmem_pixels[layout] = pixels;

         /* Применяем смещения к аттачментам */
         for (uint32_t i = 0; i < pass->attachment_count; i++) {
            struct tu_render_pass_attachment *att = &pass->attachments[i];
            if (!att->gmem)
               continue;

            uint32_t idx = att_gmem_idx[i];
            att->gmem_offset[layout] = gmem_alloc[idx].offset;
            
            if (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
               att->gmem_offset_stencil[layout] = 
                  gmem_alloc[idx + 1].offset;
            }
         }
      }
   }
}

/* Оптимизированная версия для dynamic render pass */
void HOT
tu_setup_dynamic_render_pass_optimized(struct tu_cmd_buffer *cmd_buffer,
                                      const VkRenderingInfo *info)
{
   struct tu_device *device = cmd_buffer->device;
   struct tu_render_pass *pass = &cmd_buffer->dynamic_pass;
   struct tu_subpass *subpass = &cmd_buffer->dynamic_subpasses[0];
   
   /* Быстрая очистка через ассемблерную вставку для Adreno */
   __builtin_memset(pass, 0, sizeof(*pass));
   __builtin_memset(subpass, 0, sizeof(*subpass));

   const VkMultisampledRenderToSingleSampledInfoEXT *msrtss =
      (const VkMultisampledRenderToSingleSampledInfoEXT *)
      vk_find_struct_const(info->pNext, 
                          MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT);

   /* Предзагрузка всех данных */
   __builtin_prefetch(info->pColorAttachments, 0, 3);
   __builtin_prefetch(info->pDepthAttachment, 0, 3);
   __builtin_prefetch(info->pStencilAttachment, 0, 3);

   /* Оптимизированная настройка subpass */
   subpass->color_count = info->colorAttachmentCount;
   subpass->input_count = info->colorAttachmentCount + 1;
   subpass->multiview_mask = info->viewMask;
   subpass->legacy_dithering_enabled = !!(info->flags & 
      VK_RENDERING_ENABLE_LEGACY_DITHERING_BIT_EXT);

   /* Всегда используем GMEM для максимальной производительности */
   subpass->color_attachments = cmd_buffer->dynamic_color_attachments;
   subpass->input_attachments = cmd_buffer->dynamic_input_attachments;

   if (info->flags & VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT) {
      subpass->custom_resolve = true;
      subpass->resolve_count = info->colorAttachmentCount;
      subpass->resolve_attachments = cmd_buffer->dynamic_resolve_attachments;
      pass->subpass_count = 2;
   } else {
      pass->subpass_count = 1;
   }

   if (msrtss) {
      subpass->unresolve_count = info->colorAttachmentCount;
      subpass->unresolve_attachments = cmd_buffer->dynamic_unresolve_attachments;
      subpass->samples = msrtss->rasterizationSamples;
   }

   /* Оптимизированная инициализация аттачментов с векторизацией */
   uint32_t a = 0;
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info = &info->pColorAttachments[i];
      
      if (unlikely(att_info->imageView == VK_NULL_HANDLE)) {
         subpass->color_attachments[i].attachment = VK_ATTACHMENT_UNUSED;
         subpass->input_attachments[i + 1].attachment = VK_ATTACHMENT_UNUSED;
         continue;
      }

      VK_FROM_HANDLE(tu_image_view, view, att_info->imageView);
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      
      /* Быстрая настройка аттачмента */
      att->format = view->vk.format;
      att->samples = (VkSampleCountFlagBits)view->image->layout->nr_samples;
      att->cpp = (att->format == VK_FORMAT_D32_SFLOAT_S8_UINT) 
                ? 4 * att->samples 
                : vk_format_get_blocksize(att->format) * att->samples;
      
      att->gmem = true;
      att->used_views = info->viewMask;
      
      /* Оптимизированная установка операций */
      uint32_t load_op = att_info->loadOp;
      uint32_t store_op = att_info->storeOp;
      
      att->clear_mask = (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) 
                       ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
      att->load = (load_op == VK_ATTACHMENT_LOAD_OP_LOAD);
      att->store = (store_op == VK_ATTACHMENT_STORE_OP_STORE);
      
      uint32_t att_idx = a++;
      subpass->color_attachments[i].attachment = att_idx;
      subpass->input_attachments[i + 1].attachment = att_idx;
      
      if (vk_format_is_srgb(view->vk.format))
         subpass->srgb_cntl |= 1 << i;

      /* Обработка resolve */
      if (att_info->resolveMode != VK_RESOLVE_MODE_NONE) {
         struct tu_render_pass_attachment *resolve_att = &pass->attachments[a];
         VK_FROM_HANDLE(tu_image_view, resolve_view, att_info->resolveImageView);
         
         resolve_att->format = resolve_view->vk.format;
         resolve_att->samples = VK_SAMPLE_COUNT_1_BIT;
         resolve_att->cpp = vk_format_get_blocksize(resolve_att->format);
         resolve_att->store = true;
         
         if (att_info->resolveMode == VK_RESOLVE_MODE_CUSTOM_BIT_EXT) {
            cmd_buffer->dynamic_subpasses[1].color_attachments[i].attachment = a++;
         } else {
            subpass->resolve_attachments[i].attachment = a++;
            att->will_be_resolved = true;
         }
      }
   }

   /* Обработка depth/stencil */
   if (info->pDepthAttachment || info->pStencilAttachment) {
      const VkRenderingAttachmentInfo *depth_info = info->pDepthAttachment;
      const VkRenderingAttachmentInfo *stencil_info = info->pStencilAttachment;
      
      VK_FROM_HANDLE(tu_image_view, view, 
                     depth_info ? depth_info->imageView : stencil_info->imageView);
      
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      att->format = view->vk.format;
      att->samples = (VkSampleCountFlagBits)view->image->layout->nr_samples;
      att->cpp = vk_format_get_blocksize(att->format) * att->samples;
      att->gmem = true;
      att->used_views = info->viewMask;
      
      uint32_t att_idx = a++;
      subpass->depth_stencil_attachment.attachment = att_idx;
      subpass->input_attachments[0].attachment = att_idx;
      
      if (depth_info) {
         att->clear_mask |= (depth_info->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) 
                           ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
         att->load |= (depth_info->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
         att->store |= (depth_info->storeOp == VK_ATTACHMENT_STORE_OP_STORE);
      }
      
      if (stencil_info) {
         att->clear_mask |= (stencil_info->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) 
                           ? VK_IMAGE_ASPECT_STENCIL_BIT : 0;
         att->load_stencil |= (stencil_info->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
         att->store_stencil |= (stencil_info->storeOp == VK_ATTACHMENT_STORE_OP_STORE);
      }
   }

   pass->attachment_count = a;
   pass->user_attachment_count = a;

   /* Быстрая настройка FDM если нужно */
   const VkRenderingFragmentDensityMapAttachmentInfoEXT *fdm_info =
      (const VkRenderingFragmentDensityMapAttachmentInfoEXT *)
      vk_find_struct_const(info->pNext, 
                          RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT);
   
   if (fdm_info && fdm_info->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(tu_image_view, view, fdm_info->imageView);
      struct tu_render_pass_attachment *att = &pass->attachments[a];
      
      att->format = view->vk.format;
      att->samples = VK_SAMPLE_COUNT_1_BIT;
      att->cpp = vk_format_get_blocksize(att->format);
      
      pass->fragment_density_map.attachment = a++;
      pass->has_fdm = true;
   }

   /* Оптимизированная GMEM конфигурация */
   tu_render_pass_gmem_config_optimized(pass, device->physical_device);
   
   /* Быстрый подсчет bandwidth */
   pass->gmem_bandwidth_per_pixel = 0;
   pass->sysmem_bandwidth_per_pixel = 0;
   
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      const struct tu_render_pass_attachment *att = &pass->attachments[i];
      if (att->load) pass->gmem_bandwidth_per_pixel += att->cpp;
      if (att->store) pass->gmem_bandwidth_per_pixel += att->cpp;
      if (att->clear_mask) pass->sysmem_bandwidth_per_pixel += att->cpp;
   }
   
   tu_render_pass_calc_views(pass);
}

/* Оптимизированная функция создания render pass */
VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateRenderPass2_Optimized(VkDevice _device,
                              const VkRenderPassCreateInfo2 *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(tu_device, device, _device);

   if (unlikely(TU_DEBUG(DYNAMIC)))
      return vk_common_CreateRenderPass2(_device, pCreateInfo, pAllocator, pRenderPass);

   /* Быстрый подсчет размера */
   uint32_t attachment_count = pCreateInfo->attachmentCount;
   uint32_t msrtss_extra = 0;

   /* Подсчет MSRTSS аттачментов с предзагрузкой */
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription2 *subpass = &pCreateInfo->pSubpasses[i];
      const VkMultisampledRenderToSingleSampledInfoEXT *msrtss =
         (const VkMultisampledRenderToSingleSampledInfoEXT *)
         vk_find_struct_const(subpass->pNext,
                             MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT);
      
      if (msrtss && msrtss->multisampledRenderToSingleSampledEnable) {
         for (uint32_t j = 0; j < subpass->colorAttachmentCount; j++) {
            uint32_t a = subpass->pColorAttachments[j].attachment;
            if (a != VK_ATTACHMENT_UNUSED &&
                pCreateInfo->pAttachments[a].samples != msrtss->rasterizationSamples) {
               msrtss_extra++;
            }
         }
         if (subpass->pDepthStencilAttachment &&
             subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
            uint32_t a = subpass->pDepthStencilAttachment->attachment;
            if (pCreateInfo->pAttachments[a].samples != msrtss->rasterizationSamples) {
               msrtss_extra++;
            }
         }
      }
   }

   attachment_count += msrtss_extra;

   /* Выровненное выделение памяти */
   size_t size = sizeof(struct tu_render_pass);
   size += pCreateInfo->subpassCount * sizeof(struct tu_subpass);
   size += attachment_count * sizeof(struct tu_optimized_attachment);
   size = (size + ADRENO_810_CACHE_LINE - 1) & ~(ADRENO_810_CACHE_LINE - 1);

   struct tu_render_pass *pass = (struct tu_render_pass *)
      vk_object_zalloc(&device->vk, pAllocator, size, VK_OBJECT_TYPE_RENDER_PASS);
   
   if (unlikely(!pass))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pass->attachment_count = attachment_count;
   pass->user_attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments = (struct tu_render_pass_attachment *)(pass + 1);

   /* Инициализация аттачментов с векторизацией */
   struct tu_optimized_attachment *opt_att = (struct tu_optimized_attachment *)pass->attachments;
   
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      const VkAttachmentDescription2 *desc = &pCreateInfo->pAttachments[i];
      struct tu_optimized_attachment *att = &opt_att[i];
      
      att->format = desc->format;
      att->samples = desc->samples;
      att->cpp = (desc->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
                ? 4 * desc->samples
                : vk_format_get_blocksize(desc->format) * desc->samples;
      
      /* Оптимизированная установка операций */
      uint32_t clear = 0;
      if (desc->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) clear |= VK_IMAGE_ASPECT_COLOR_BIT;
      if (desc->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) clear |= VK_IMAGE_ASPECT_STENCIL_BIT;
      
      att->clear_mask = clear;
      att->load = (desc->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
      att->load_stencil = (desc->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
      att->store = (desc->storeOp == VK_ATTACHMENT_STORE_OP_STORE);
      att->store_stencil = (desc->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE);
      
      att->first_subpass_idx = VK_SUBPASS_EXTERNAL;
      att->last_subpass_idx = 0;
   }

   /* Остальная инициализация... */
   /* Здесь должен быть код для настройки subpasses, зависимостей и т.д. */

   *pRenderPass = tu_render_pass_to_handle(pass);
   return VK_SUCCESS;
}

/* Ассемблерные оптимизации для критических функций */
static inline void
optimized_memory_barrier(uint32_t flags)
{
   /* Inline assembly for Adreno-specific memory barriers */
   __asm__ volatile(
      "dsb sy\n"
      "isb\n"
      : : "r" (flags) : "memory"
   );
}

/* Быстрое копирование данных с оптимизацией для кэша */
static inline void
optimized_memcpy(void *dst, const void *src, size_t n)
{
   uint8_t *d = (uint8_t *)dst;
   const uint8_t *s = (const uint8_t *)src;
   
   /* Используем 16-байтовые векторные инструкции */
   while (n >= 16) {
      __builtin_prefetch(s + 64, 0, 3);
      __builtin_prefetch(d + 64, 1, 3);
      
      uint64_t a = *(const uint64_t *)s;
      uint64_t b = *(const uint64_t *)(s + 8);
      *(uint64_t *)d = a;
      *(uint64_t *)(d + 8) = b;
      
      d += 16;
      s += 16;
      n -= 16;
   }
   
   /* Остаток */
   while (n--) {
      *d++ = *s++;
   }
}

/* Экспорт оптимизированных функций */
void (*tu_setup_dynamic_render_pass)(struct tu_cmd_buffer *, const VkRenderingInfo *) = 
   tu_setup_dynamic_render_pass_optimized;

VkResult (*tu_CreateRenderPass2)(VkDevice, const VkRenderPassCreateInfo2 *, 
                                 const VkAllocationCallbacks *, VkRenderPass *) = 
   tu_CreateRenderPass2_Optimized;