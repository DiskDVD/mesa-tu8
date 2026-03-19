/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its susidiaries.
 * SPDX-License-Identifier: MIT
 */

#ifndef FD6_GMEM_CACHE_H
#define FD6_GMEM_CACHE_H

#include <stdint.h>

#include "common/freedreno_dev_info.h"

/* Offset within GMEM of various "non-GMEM" things that GMEM is used to
 * cache.  These offsets differ for gmem vs sysmem rendering (in sysmem
 * mode, the entire GMEM can be used)
 */
struct fd6_gmem_config {
   /* Color/depth CCU cache: */
   uint32_t color_ccu_offset;
   uint32_t depth_ccu_offset;

   /* Vertex attrib cache (a750+): */
   uint32_t vpc_attr_buf_size;
   uint32_t vpc_attr_buf_offset;

   /* Vertex position cache (a8xx+): */
   uint32_t vpc_pos_buf_size;
   uint32_t vpc_pos_buf_offset;
   uint32_t vpc_bv_pos_buf_size;
   uint32_t vpc_bv_pos_buf_offset;

   /* see enum a6xx_ccu_cache_size */
   uint32_t depth_cache_fraction;
   uint32_t color_cache_fraction;
   uint32_t depth_cache_size;
   uint32_t color_cache_size;
};

static inline unsigned
__calc_gmem_cache_offsets(const struct fd_dev_info *info, unsigned offset,
                          struct fd6_gmem_config *config)
{
   unsigned num_ccu = info->num_ccu;

   /* This seems not to be load bearing, but keeping it for now to match blob: */
   if (info->chip >= 8 && info->num_slices > 1)
      //offset -= 0x78000;
   {}

   config->vpc_bv_pos_buf_offset = offset - (num_ccu * config->vpc_bv_pos_buf_size);
   offset = config->vpc_bv_pos_buf_offset;

   config->vpc_attr_buf_offset = offset - (num_ccu * config->vpc_attr_buf_size);
   offset = config->vpc_attr_buf_offset;

   config->vpc_pos_buf_offset = offset - (num_ccu * config->vpc_pos_buf_size);
   offset = config->vpc_pos_buf_offset;

   config->color_ccu_offset = offset - (num_ccu * config->color_cache_size);
   offset = config->color_ccu_offset;

   config->depth_ccu_offset = offset - (num_ccu * config->depth_cache_size);
   offset = config->depth_ccu_offset;

   return offset;
}

static inline unsigned
fd6_calc_gmem_cache_offsets(const struct fd_dev_info *info, unsigned gmemsize_bytes,
                            struct fd6_gmem_config *gmem, struct fd6_gmem_config *sysmem)
{
   uint32_t depth_cache_size =
      info->num_ccu * info->props.sysmem_per_ccu_depth_cache_size;
   uint32_t color_cache_size =
      (info->num_ccu * info->props.sysmem_per_ccu_color_cache_size);
   uint32_t color_cache_size_gmem =
      color_cache_size /
      (1 << info->props.gmem_ccu_color_cache_fraction);

   sysmem->depth_ccu_offset = 0;
   sysmem->color_ccu_offset = sysmem->depth_ccu_offset + depth_cache_size;

   /* ===== A810: ОПТИМИЗАЦИЯ ПОД 576KB GMEM ===== */
   if (info->chip >= 8 && info->num_ccu == 2) {
      /* A810 имеет 576KB GMEM, а не 512KB как думали раньше */
      gmemsize_bytes = 576 * 1024;
      
      /* Логируем реальный размер для отладки */
      static bool logged = false;
      if (!logged) {
         mesa_logi("A810: GMEM size set to 576KB");
         logged = true;
      }
   }
   /* ===== КОНЕЦ ===== */

   /* TODO we could unify gen7/gen8 setup.. gen7 is a subset.. */
   if (info->chip == 8) {
      gmem->depth_cache_fraction = info->props.gmem_ccu_depth_cache_fraction;
      gmem->depth_cache_size     = info->props.gmem_per_ccu_depth_cache_size;
      gmem->color_cache_fraction = info->props.gmem_ccu_color_cache_fraction;
      gmem->color_cache_size     = info->props.gmem_per_ccu_color_cache_size;
      gmem->vpc_attr_buf_size    = info->props.gmem_vpc_attr_buf_size;
      gmem->vpc_pos_buf_size     = info->props.gmem_vpc_pos_buf_size;
      gmem->vpc_bv_pos_buf_size  = info->props.gmem_vpc_bv_pos_buf_size;

      sysmem->depth_cache_fraction = info->props.sysmem_ccu_depth_cache_fraction;
      sysmem->depth_cache_size     = info->props.sysmem_per_ccu_depth_cache_size;
      sysmem->color_cache_fraction = info->props.sysmem_ccu_color_cache_fraction;
      sysmem->color_cache_size     = info->props.sysmem_per_ccu_color_cache_size;
      sysmem->vpc_attr_buf_size    = info->props.sysmem_vpc_attr_buf_size;
      sysmem->vpc_pos_buf_size     = info->props.sysmem_vpc_pos_buf_size;
      sysmem->vpc_bv_pos_buf_size  = info->props.sysmem_vpc_bv_pos_buf_size;

      __calc_gmem_cache_offsets(info, gmemsize_bytes, sysmem);
      return __calc_gmem_cache_offsets(info, gmemsize_bytes, gmem);
   } else if (info->props.has_gmem_vpc_attr_buf) {
      sysmem->vpc_attr_buf_size = info->props.sysmem_vpc_attr_buf_size;
      sysmem->vpc_attr_buf_offset = sysmem->color_ccu_offset + color_cache_size;

      /* ===== ЗАЩИТА ОТ UNDERFLOW ДЛЯ A810 И ДРУГИХ ЧИПОВ ===== */
      gmem->vpc_attr_buf_size = info->props.gmem_vpc_attr_buf_size;
      
      /* Проверка переполнения VPC буфера */
      uint32_t vpc_total = gmem->vpc_attr_buf_size * info->num_ccu;
      if (vpc_total > gmemsize_bytes) {
         /* Если не влезает - уменьшаем до 75% GMEM */
         uint32_t max_vpc = gmemsize_bytes * 3 / 4;  // 75% от GMEM
         gmem->vpc_attr_buf_size = max_vpc / info->num_ccu;
         /* Выравниваем для безопасности */
         gmem->vpc_attr_buf_size = (gmem->vpc_attr_buf_size / 1024) * 1024;
         vpc_total = gmem->vpc_attr_buf_size * info->num_ccu;
         
         /* Логируем только для A810, чтобы не засорять другие чипы */
         if (info->chip >= 8 && info->num_ccu == 2) {
            mesa_logw("GMEM: VPC buffer reduced to %u KB (was %u KB) due to size constraints",
                      gmem->vpc_attr_buf_size / 1024,
                      info->props.gmem_vpc_attr_buf_size / 1024);
         }
      }
      
      /* Расчёт смещения VPC буфера */
      gmem->vpc_attr_buf_offset = gmemsize_bytes - vpc_total;
      
      /* Проверка переполнения цветового кэша */
      uint32_t color_ccu_total = color_cache_size_gmem;
      if (gmem->vpc_attr_buf_offset < color_ccu_total) {
         /* Если не влезает - уменьшаем цветовой кэш */
         color_ccu_total = gmem->vpc_attr_buf_offset;
         gmem->color_cache_size = color_ccu_total / info->num_ccu;
         
         if (info->chip >= 8 && info->num_ccu == 2) {
            mesa_logw("GMEM: Color CCU reduced to %u KB per CCU due to space constraints",
                      gmem->color_cache_size / 1024);
         }
      }
      
      /* Проверка переполнения depth кэша */
      uint32_t depth_ccu_total = gmem->depth_cache_size * info->num_ccu;
      uint32_t remaining = gmem->vpc_attr_buf_offset - color_ccu_total;
      if (remaining < depth_ccu_total) {
         /* Если не влезает - уменьшаем depth кэш */
         gmem->depth_cache_size = remaining / info->num_ccu;
         gmem->depth_cache_size = (gmem->depth_cache_size / 1024) * 1024;
         depth_ccu_total = gmem->depth_cache_size * info->num_ccu;
         
         if (info->chip >= 8 && info->num_ccu == 2) {
            mesa_logw("GMEM: Depth CCU reduced to %u KB per CCU due to space constraints",
                      gmem->depth_cache_size / 1024);
         }
      }
      
      /* Финальный расчёт смещений */
      gmem->color_ccu_offset = gmem->vpc_attr_buf_offset - color_ccu_total;
      gmem->depth_ccu_offset = gmem->color_ccu_offset - depth_ccu_total;
      
      /* Финальная проверка: всё должно быть положительно */
      if (gmem->depth_ccu_offset < 0) {
         /* Если всё равно отрицательно - экстренное восстановление */
         mesa_logw("GMEM: Emergency recovery - resetting all caches");
         
         /* Распределяем равномерно */
         uint32_t per_ccu = gmemsize_bytes / (info->num_ccu * 3);
         gmem->color_cache_size = per_ccu;
         gmem->depth_cache_size = per_ccu;
         gmem->vpc_attr_buf_size = per_ccu;
         
         gmem->vpc_attr_buf_offset = gmemsize_bytes - (per_ccu * info->num_ccu);
         gmem->color_ccu_offset = gmem->vpc_attr_buf_offset - (per_ccu * info->num_ccu);
         gmem->depth_ccu_offset = gmem->color_ccu_offset - (per_ccu * info->num_ccu);
      }
      
      /* Для отладки - логируем итоговую конфигурацию A810 */
      if (info->chip >= 8 && info->num_ccu == 2) {
         static bool config_logged = false;
         if (!config_logged) {
            mesa_logi("A810 GMEM config: VPC=%uKB, Color=%uKB, Depth=%uKB, Free=%dKB",
                      gmem->vpc_attr_buf_size / 1024,
                      gmem->color_cache_size / 1024,
                      gmem->depth_cache_size / 1024,
                      (gmem->depth_ccu_offset) / 1024);
            config_logged = true;
         }
      }
      /* ===== КОНЕЦ ЗАЩИТЫ ===== */

      return gmem->vpc_attr_buf_offset;
   } else {
      gmem->depth_ccu_offset = 0;
      gmem->color_ccu_offset = gmemsize_bytes - color_cache_size_gmem;

      return gmemsize_bytes;
   }
}

#endif /* FD6_GMEM_CACHE_H */
