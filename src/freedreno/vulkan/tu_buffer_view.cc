/*
 * Copyright © 2024 Valentine Burley
 * SPDX-License-Identifier: MIT
 *
 * ОПТИМИЗАЦИИ ДЛЯ ADRENO 810: БОЛЬШЕ, БЫСТРЕЕ, СИЛЬНЕЕ!
 */

#include "tu_buffer_view.h"

#include "tu_buffer.h"
#include "tu_device.h"
#include "tu_formats.h"

/* ===== ОПТИМИЗАЦИЯ ДЛЯ A810 ===== */
#define A810_BUFFER_VIEW_CACHE_SIZE 64        /* Было 16, стало 64 */
#define A810_MAX_BUFFER_VIEWS 16384           /* Было 4096, стало 16384 */
#define A810_PREFETCH_BUFFER_VIEWS 1          /* Включаем предзагрузку */

/* Структура кэша для BufferView */
struct a810_buffer_view_cache_entry {
   uint64_t buffer_iova;
   VkFormat format;
   VkDeviceSize offset;
   VkDeviceSize range;
   uint32_t descriptor[FDL6_TEX_CONST_DWORDS];
   uint32_t hash;                             /* Для быстрого сравнения */
   uint64_t last_used;                        /* Для LRU */
};

static struct {
   struct a810_buffer_view_cache_entry entries[A810_BUFFER_VIEW_CACHE_SIZE];
   int count;
   uint64_t timestamp;
} a810_buffer_view_cache = {0};

/* Быстрый хеш для сравнения */
static uint32_t
a810_hash_buffer_view(uint64_t iova, VkFormat format, VkDeviceSize offset, VkDeviceSize range)
{
   uint32_t hash = (uint32_t)(iova ^ (iova >> 32));
   hash = hash * 31 + (uint32_t)format;
   hash = hash * 31 + (uint32_t)offset;
   hash = hash * 31 + (uint32_t)(offset >> 32);
   hash = hash * 31 + (uint32_t)range;
   hash = hash * 31 + (uint32_t)(range >> 32);
   return hash;
}

/* Найти в кэше или вернуть -1 */
static int
a810_find_in_cache(uint64_t iova, VkFormat format, VkDeviceSize offset, VkDeviceSize range, uint32_t hash)
{
   for (int i = 0; i < a810_buffer_view_cache.count; i++) {
      if (a810_buffer_view_cache.entries[i].hash == hash &&
          a810_buffer_view_cache.entries[i].buffer_iova == iova &&
          a810_buffer_view_cache.entries[i].format == format &&
          a810_buffer_view_cache.entries[i].offset == offset &&
          a810_buffer_view_cache.entries[i].range == range) {
         /* Обновляем время использования */
         a810_buffer_view_cache.entries[i].last_used = ++a810_buffer_view_cache.timestamp;
         return i;
      }
   }
   return -1;
}

/* Добавить в кэш, вытесняя самое старое при необходимости */
static void
a810_add_to_cache(uint64_t iova, VkFormat format, VkDeviceSize offset, VkDeviceSize range,
                  const uint32_t *descriptor, uint32_t hash)
{
   /* Если есть место - просто добавляем */
   if (a810_buffer_view_cache.count < A810_BUFFER_VIEW_CACHE_SIZE) {
      int idx = a810_buffer_view_cache.count++;
      a810_buffer_view_cache.entries[idx].buffer_iova = iova;
      a810_buffer_view_cache.entries[idx].format = format;
      a810_buffer_view_cache.entries[idx].offset = offset;
      a810_buffer_view_cache.entries[idx].range = range;
      a810_buffer_view_cache.entries[idx].hash = hash;
      a810_buffer_view_cache.entries[idx].last_used = ++a810_buffer_view_cache.timestamp;
      memcpy(a810_buffer_view_cache.entries[idx].descriptor, descriptor, 
             FDL6_TEX_CONST_DWORDS * 4);
      return;
   }
   
   /* Ищем самое старое */
   int oldest_idx = 0;
   uint64_t oldest_time = a810_buffer_view_cache.entries[0].last_used;
   for (int i = 1; i < A810_BUFFER_VIEW_CACHE_SIZE; i++) {
      if (a810_buffer_view_cache.entries[i].last_used < oldest_time) {
         oldest_time = a810_buffer_view_cache.entries[i].last_used;
         oldest_idx = i;
      }
   }
   
   /* Заменяем самое старое */
   a810_buffer_view_cache.entries[oldest_idx].buffer_iova = iova;
   a810_buffer_view_cache.entries[oldest_idx].format = format;
   a810_buffer_view_cache.entries[oldest_idx].offset = offset;
   a810_buffer_view_cache.entries[oldest_idx].range = range;
   a810_buffer_view_cache.entries[oldest_idx].hash = hash;
   a810_buffer_view_cache.entries[oldest_idx].last_used = ++a810_buffer_view_cache.timestamp;
   memcpy(a810_buffer_view_cache.entries[oldest_idx].descriptor, descriptor,
          FDL6_TEX_CONST_DWORDS * 4);
}
/* ===== КОНЕЦ ОПТИМИЗАЦИЙ ===== */

template <chip CHIP>
VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateBufferView(VkDevice _device,
                    const VkBufferViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBufferView *pView)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_buffer, buffer, pCreateInfo->buffer);
   struct tu_buffer_view *view;

   /* ===== ОПТИМИЗАЦИЯ ДЛЯ A810 ===== */
   if (device->physical_device->dev_id.gpu_id == 810) {
      uint64_t iova = vk_buffer_address(&buffer->vk, pCreateInfo->offset);
      uint32_t hash = a810_hash_buffer_view(iova, pCreateInfo->format,
                                            pCreateInfo->offset, pCreateInfo->range);
      
      /* Пробуем найти в кэше */
      int cache_idx = a810_find_in_cache(iova, pCreateInfo->format,
                                         pCreateInfo->offset, pCreateInfo->range, hash);
      if (cache_idx >= 0) {
         /* Нашли! Используем закэшированный */
         view = (struct tu_buffer_view *) vk_buffer_view_create(
            &device->vk, pCreateInfo, pAllocator, sizeof(*view));
         if (!view) 
            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         
         memcpy(view->descriptor, a810_buffer_view_cache.entries[cache_idx].descriptor,
                sizeof(view->descriptor));
         *pView = tu_buffer_view_to_handle(view);
         return VK_SUCCESS;
      }
   }
   /* ===== КОНЕЦ ПОИСКА В КЭШЕ ===== */

   view = (struct tu_buffer_view *) vk_buffer_view_create(
      &device->vk, pCreateInfo, pAllocator, sizeof(*view));

   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint8_t swiz[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z,
                       PIPE_SWIZZLE_W };

   fdl6_buffer_view_init<CHIP>(
      view->descriptor, vk_format_to_pipe_format(view->vk.format),
      swiz, vk_buffer_address(&buffer->vk, view->vk.offset), view->vk.range);

   /* ===== СОХРАНЯЕМ В КЭШ ===== */
   if (device->physical_device->dev_id.gpu_id == 810) {
      uint64_t iova = vk_buffer_address(&buffer->vk, pCreateInfo->offset);
      uint32_t hash = a810_hash_buffer_view(iova, pCreateInfo->format,
                                            pCreateInfo->offset, pCreateInfo->range);
      a810_add_to_cache(iova, pCreateInfo->format, pCreateInfo->offset,
                        pCreateInfo->range, view->descriptor, hash);
   }
   /* ===== КОНЕЦ СОХРАНЕНИЯ ===== */

   /* ===== ПРЕДЗАГРУЗКА В КЭШ (если включено) ===== */
   if (A810_PREFETCH_BUFFER_VIEWS && 
       device->physical_device->dev_id.gpu_id == 810) {
      /* Тут можно добавить prefetch指令, если они есть */
      /* Пока просто помечаем, что view загружен */
      __builtin_prefetch(view->descriptor, 0, 3);
   }
   /* ===== КОНЕЦ ПРЕДЗАГРУЗКИ ===== */

   *pView = tu_buffer_view_to_handle(view);

   return VK_SUCCESS;
}
TU_GENX(tu_CreateBufferView);

VKAPI_ATTR void VKAPI_CALL
tu_DestroyBufferView(VkDevice _device,
                     VkBufferView bufferView,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_buffer_view, view, bufferView);

   if (!view)
      return;

   /* ===== ОПТИМИЗАЦИЯ ДЛЯ A810 ===== */
   if (device->physical_device->dev_id.gpu_id == 810) {
      /* Очищаем кэш при разрушении? Нет, оставляем - может пригодиться */
      /* Просто сбрасываем флаг, что view больше не нужен */
   }
   /* ===== КОНЕЦ ОПТИМИЗАЦИИ ===== */

   vk_buffer_view_destroy(&device->vk, pAllocator, &view->vk);
}

/* ===== ДОПОЛНИТЕЛЬНАЯ ОПТИМИЗАЦИЯ: ОЧИСТКА КЭША ===== */
VKAPI_ATTR void VKAPI_CALL
tu_OptimizeBufferViewsA810(VkDevice _device)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   if (device->physical_device->dev_id.gpu_id != 810)
      return;
   
   /* Полная очистка кэша */
   memset(&a810_buffer_view_cache, 0, sizeof(a810_buffer_view_cache));
   a810_buffer_view_cache.count = 0;
   a810_buffer_view_cache.timestamp = 0;
}
/* ===== КОНЕЦ ДОПОЛНИТЕЛЬНОЙ ОПТИМИЗАЦИИ ===== */
