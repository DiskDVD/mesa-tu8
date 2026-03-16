/*
 * Copyright © 2016 Red Hat
 * SPDX-License-Identifier: MIT
 *
 * based on intel anv code:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_wsi.h"

#include "vk_util.h"
#include "wsi_common_drm.h"
#include "drm-uapi/drm_fourcc.h"

#include "tu_device.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
tu_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

static bool
tu_wsi_can_present_on_device(VkPhysicalDevice physicalDevice, int fd)
{
#ifdef HAVE_LIBDRM
   VK_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);
   return wsi_common_drm_devices_equal(fd, pdevice->local_fd);
#else
   return true;
#endif
}

VkResult
tu_wsi_init(struct tu_physical_device *physical_device)
{
   VkResult result;

   const struct wsi_device_options options = { .sw_device = false };
   result = wsi_device_init(&physical_device->wsi_device,
                            tu_physical_device_to_handle(physical_device),
                            tu_wsi_proc_addr,
                            &physical_device->instance->vk.alloc,
                            physical_device->master_fd,
                            &physical_device->instance->dri_options,
                            &options);
   if (result != VK_SUCCESS)
      return result;

   physical_device->wsi_device.supports_modifiers = true;
   physical_device->wsi_device.can_present_on_device =
      tu_wsi_can_present_on_device;

   /* ===== ОПТИМИЗАЦИИ ДЛЯ ADRENO 810 ===== */
   if (physical_device->dev_id.gpu_id == 810) {
      /* A810: форсируем оптимальные модификаторы */
      physical_device->wsi_device.force_modifiers = true;
      physical_device->wsi_device.prefer_modifiers = true;
      
      /* A810: меньше буферов = меньше RAM */
      physical_device->wsi_device.min_image_count = 2;
      physical_device->wsi_device.max_image_count = 3;
      
      /* A810: подсказки для маленького GMEM */
      physical_device->wsi_device.small_gmem = true;
      physical_device->wsi_device.prefer_16bit_formats = true;
      
      /* A810: предпочитаем быстрые форматы */
      physical_device->wsi_device.prefer_fast_formats = true;
      
      /* A810: уменьшаем таймауты для более быстрого отклика */
      physical_device->wsi_device.present_wait_timeout = 1000000; // 1ms
   }
   /* ===== КОНЕЦ ОПТИМИЗАЦИЙ ===== */

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   return VK_SUCCESS;
}

void
tu_wsi_finish(struct tu_physical_device *physical_device)
{
   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device,
                     &physical_device->instance->vk.alloc);
}
