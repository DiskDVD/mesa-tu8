/*
 * Copyright © 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

/**
 * Оптимизированный субадлокатор для Adreno 810.
 *
 * Реальные флаги из tu_knl.h:
 * TU_BO_ALLOC_NO_FLAGS = 0
 * TU_BO_ALLOC_ALLOW_DUMP = 1 << 0
 * TU_BO_ALLOC_CPU_PREP = 1 << 1
 * TU_BO_ALLOC_DMABUF = 1 << 4
 */

#include "tu_suballoc.h"
#include "util/u_math.h"

/* Размер кэш-линии для Adreno 810 */
#define ADRENO_CACHE_LINE_SIZE 64

/* Порог для переключения типа памяти (условно, так как нет прямых флагов VRAM/SYSTEM) */
#define ADRENO_LARGE_ALLOC_THRESHOLD (512 * 1024) /* 512KB */

/* Максимальный размер для пула быстрых аллокаций */
#define ADRENO_FAST_POOL_MAX (2 * 1024 * 1024) /* 2MB */

void
tu_bo_suballocator_init(struct tu_suballocator *suballoc,
                        struct tu_device *dev,
                        uint32_t default_size,
                        enum tu_bo_alloc_flags flags,
                        const char *name)
{
   suballoc->dev = dev;
   
   /* Snapdragon 6 Gen 4: увеличиваем размер по умолчанию */
   suballoc->default_size = MAX2(default_size, ADRENO_FAST_POOL_MAX / 3);
   
   /* Добавляем CPU_PREP для кэширования (реальный флаг) */
   if (!(flags & TU_BO_ALLOC_CPU_PREP)) {
      flags |= TU_BO_ALLOC_CPU_PREP;
   }
   
   suballoc->flags = flags;
   suballoc->bo = NULL;
   suballoc->cached_bo = NULL;
   suballoc->name = name;
   
   /* Добавляем поля в структуру (нужно определить в tu_suballoc.h) */
   /* Если их нет в заголовке - закомментируйте */
   #ifdef TU_SUBALLOC_HAS_STATS
   suballoc->total_allocated = 0;
   suballoc->total_wasted = 0;
   suballoc->allocation_count = 0;
   #endif
}

void
tu_bo_suballocator_finish(struct tu_suballocator *suballoc)
{
   if (suballoc->bo) {
      tu_bo_finish(suballoc->dev, suballoc->bo);
      suballoc->bo = NULL;
   }
   
   if (suballoc->cached_bo) {
      tu_bo_finish(suballoc->dev, suballoc->cached_bo);
      suballoc->cached_bo = NULL;
   }
}

static inline uint32_t
adreno_align_size(uint32_t size)
{
   /* Adreno 810: выравнивание по 64 байт */
   return align(size, ADRENO_CACHE_LINE_SIZE);
}

VkResult
tu_suballoc_bo_alloc(struct tu_suballoc_bo *suballoc_bo,
                     struct tu_suballocator *suballoc,
                     uint32_t size, uint32_t alignment)
{
   /* Выравниваем размер и alignment */
   size = adreno_align_size(size);
   alignment = MAX2(alignment, ADRENO_CACHE_LINE_SIZE);
   
   struct tu_bo *bo = suballoc->bo;
   
   if (bo) {
      uint32_t offset = align(suballoc->next_offset, alignment);
      
      if (offset + size <= bo->size) {
         suballoc_bo->bo = tu_bo_get_ref(bo);
         suballoc_bo->iova = bo->iova + offset;
         suballoc_bo->size = size;
         
         /* Сохраняем offset для map (можно хранить временно) */
         /* В оригинале нет offset_in_bo, используем для map позже */
         
         suballoc->next_offset = offset + size;
         
         #ifdef TU_SUBALLOC_HAS_STATS
         suballoc->total_allocated += size;
         #endif
         
         return VK_SUCCESS;
      }
      
      /* Если не влезло - освобождаем BO */
      tu_bo_finish(suballoc->dev, bo);
      suballoc->bo = NULL;
   }
   
   /* Расчет оптимального размера аллокации */
   uint32_t alloc_size = MAX2(size, suballoc->default_size);
   
   /* Для больших аллокаций увеличиваем размер */
   if (size >= ADRENO_LARGE_ALLOC_THRESHOLD) {
      alloc_size = MAX2(alloc_size, ADRENO_FAST_POOL_MAX);
   }
   
   /* Используем кэшированный BO если подходит */
   if (suballoc->cached_bo) {
      if (alloc_size <= suballoc->cached_bo->size) {
         suballoc->bo = suballoc->cached_bo;
         suballoc->cached_bo = NULL;
      } else {
         tu_bo_finish(suballoc->dev, suballoc->cached_bo);
         suballoc->cached_bo = NULL;
      }
   }
   
   /* Создаем новый BO */
   if (!suballoc->bo) {
      VkResult result = tu_bo_init_new(suballoc->dev, NULL,
                                       &suballoc->bo, alloc_size,
                                       suballoc->flags, suballoc->name);
      if (result != VK_SUCCESS) {
         return result;
      }
      
      /* Маппим BO для доступа CPU */
      result = tu_bo_map(suballoc->dev, suballoc->bo, NULL);
      if (result != VK_SUCCESS) {
         tu_bo_finish(suballoc->dev, suballoc->bo);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   
   suballoc_bo->bo = tu_bo_get_ref(suballoc->bo);
   suballoc_bo->iova = suballoc_bo->bo->iova;
   suballoc_bo->size = size;
   suballoc->next_offset = size;
   
   #ifdef TU_SUBALLOC_HAS_STATS
   suballoc->total_allocated += size;
   suballoc->allocation_count++;
   #endif
   
   return VK_SUCCESS;
}

void
tu_suballoc_bo_free(struct tu_suballocator *suballoc, struct tu_suballoc_bo *bo)
{
   if (!bo || !bo->bo) {
      return;
   }
   
   /* Проверяем refcnt (оригинальный код) */
   if (p_atomic_read(&bo->bo->refcnt) == 1 && !suballoc->cached_bo) {
      /* Кэшируем BO */
      suballoc->cached_bo = bo->bo;
      
      #ifdef TU_SUBALLOC_HAS_STATS
      /* Считаем wasted space при кэшировании */
      if (suballoc->bo && suballoc->next_offset < suballoc->bo->size) {
         suballoc->total_wasted += (suballoc->bo->size - suballoc->next_offset);
      }
      #endif
      
      return;
   }
   
   /* Обычное освобождение */
   tu_bo_finish(suballoc->dev, bo->bo);
}

void *
tu_suballoc_bo_map(struct tu_suballoc_bo *bo)
{
   if (!bo || !bo->bo || !bo->bo->map) {
      return NULL;
   }
   
   /* В оригинале iova может отличаться от bo->iova, если это субучасток */
   /* Вычисляем смещение правильно */
   return (uint8_t *)bo->bo->map + (bo->iova - bo->bo->iova);
}

/* Добавляем функцию очистки, если нужна */
VkResult
tu_suballocator_trim(struct tu_suballocator *suballoc)
{
   /* Очищаем кэшированный BO */
   if (suballoc->cached_bo) {
      tu_bo_finish(suballoc->dev, suballoc->cached_bo);
      suballoc->cached_bo = NULL;
   }
   
   return VK_SUCCESS;
}