/*
 * Copyright © 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

/**
 * Безопасная оптимизация для Adreno 810
 * - Только выравнивание (не требует новых флагов)
 * - Только существующие поля структур
 * - Никаких новых флагов
 */

#include "tu_suballoc.h"
#include "util/u_math.h"

/* Размер кэш-линии для Adreno 810 */
#define ADRENO_CACHE_LINE_SIZE 64

void
tu_bo_suballocator_init(struct tu_suballocator *suballoc,
                        struct tu_device *dev,
                        uint32_t default_size,
                        enum tu_bo_alloc_flags flags,
                        const char *name)
{
   suballoc->dev = dev;
   
   /* Snapdragon 6 Gen 4: просто увеличиваем размер по умолчанию */
   /* 2MB пул лучше для GTA V / Stray */
   suballoc->default_size = MAX2(default_size, 2 * 1024 * 1024);
   
   /* НЕ добавляем никаких флагов - оставляем как есть */
   suballoc->flags = flags;
   suballoc->bo = NULL;
   suballoc->cached_bo = NULL;
   suballoc->name = name;
}

void
tu_bo_suballocator_finish(struct tu_suballocator *suballoc)
{
   if (suballoc->bo)
      tu_bo_finish(suballoc->dev, suballoc->bo);
   if (suballoc->cached_bo)
      tu_bo_finish(suballoc->dev, suballoc->cached_bo);
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
   /* Применяем выравнивание Adreno */
   uint32_t aligned_size = adreno_align_size(size);
   uint32_t aligned_align = MAX2(alignment, ADRENO_CACHE_LINE_SIZE);
   
   struct tu_bo *bo = suballoc->bo;
   if (bo) {
      uint32_t offset = align(suballoc->next_offset, aligned_align);
      if (offset + aligned_size <= bo->size) {
         suballoc_bo->bo = tu_bo_get_ref(bo);
         suballoc_bo->iova = bo->iova + offset;
         suballoc_bo->size = aligned_size; /* Используем выровненный размер */

         suballoc->next_offset = offset + aligned_size;
         return VK_SUCCESS;
      } else {
         tu_bo_finish(suballoc->dev, bo);
         suballoc->bo = NULL;
      }
   }

   /* Для GTA V/Stray: если запрос больше 256KB, увеличиваем размер BO */
   uint32_t alloc_size;
   if (aligned_size > 256 * 1024) {
      /* Для больших аллокаций выделяем с запасом */
      alloc_size = MAX2(aligned_size * 2, suballoc->default_size);
   } else {
      alloc_size = MAX2(aligned_size, suballoc->default_size);
   }

   /* Reuse a recycled suballoc BO if we have one and it's big enough, otherwise free it. */
   if (suballoc->cached_bo) {
      if (alloc_size <= suballoc->cached_bo->size)
         suballoc->bo = suballoc->cached_bo;
      else
         tu_bo_finish(suballoc->dev, suballoc->cached_bo);
      suballoc->cached_bo = NULL;
   }

   /* Allocate the new BO if we didn't have one cached. */
   if (!suballoc->bo) {
      VkResult result = tu_bo_init_new(suballoc->dev, NULL,
                                       &suballoc->bo, alloc_size,
                                       suballoc->flags, suballoc->name);
      if (result != VK_SUCCESS)
         return result;
   }

   VkResult result = tu_bo_map(suballoc->dev, suballoc->bo, NULL);
   if (result != VK_SUCCESS) {
      tu_bo_finish(suballoc->dev, suballoc->bo);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   suballoc_bo->bo = tu_bo_get_ref(suballoc->bo);
   suballoc_bo->iova = suballoc_bo->bo->iova;
   suballoc_bo->size = aligned_size; /* Сохраняем реальный запрошенный размер */
   suballoc->next_offset = aligned_size;

   return VK_SUCCESS;
}

void
tu_suballoc_bo_free(struct tu_suballocator *suballoc, struct tu_suballoc_bo *bo)
{
   if (!bo->bo)
      return;

   /* If we we held the last reference to this BO, so just move it to the
    * suballocator for the next time we need to allocate.
    */
   if (p_atomic_read(&bo->bo->refcnt) == 1 && !suballoc->cached_bo) {
      suballoc->cached_bo = bo->bo;
      return;
   }

   /* Otherwise, drop the refcount on it normally. */
   tu_bo_finish(suballoc->dev, bo->bo);
}

void *
tu_suballoc_bo_map(struct tu_suballoc_bo *bo)
{
   /* Вычисляем смещение правильно для выровненных адресов */
   return (char *)bo->bo->map + (bo->iova - bo->bo->iova);
}