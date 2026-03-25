/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 */

#include "tu_formats.h"

#include "fdl/fd6_format_table.h"
#include "common/freedreno_ubwc.h"

#include "vk_android.h"
#include "vk_enum_defines.h"
#include "vk_util.h"
#include "vk_acceleration_structure.h"
#include "drm-uapi/drm_fourcc.h"

#include "tu_device.h"
#include "tu_image.h"

static bool
tu6_format_vtx_supported(enum pipe_format format)
{
   return fd6_vertex_format(format) != FMT6_NONE;
}

struct tu_native_format
tu6_format_vtx(enum pipe_format format)
{
   struct tu_native_format fmt = {
      .fmt = fd6_vertex_format(format),
      .swap = fd6_vertex_swap(format),
   };
   assert(tu6_format_vtx_supported(format));
   return fmt;
}

static bool
tu6_format_color_supported(const struct fd_dev_info *info, enum pipe_format format)
{
   return fd6_color_format_supported(info, format, TILE6_LINEAR);
}

struct tu_native_format
tu6_format_color(enum pipe_format format, enum a6xx_tile_mode tile_mode,
                 bool is_mutable)
{
   struct tu_native_format fmt = {
      .fmt = fd6_color_format(format, tile_mode),
      .swap = fd6_color_swap(format, tile_mode, is_mutable),
   };
   assert(fmt.fmt != FMT6_NONE);
   return fmt;
}

struct tu_native_format
tu6_format_texture(enum pipe_format format, enum a6xx_tile_mode tile_mode,
                   bool is_mutable)
{
   struct tu_native_format fmt = {
      .fmt = fd6_texture_format(format, tile_mode, is_mutable),
      .swap = fd6_texture_swap(format, tile_mode, is_mutable),
   };
   assert(fmt.fmt != FMT6_NONE);
   return fmt;
}

/* БЕЗОПАСНАЯ ВЕРСИЯ — БЕЗ СПЕЦИАЛЬНЫХ ПРАВОК ДЛЯ A810 */
static enum fd6_ubwc_compat_type
tu6_ubwc_compat_mode(const struct fd_dev_info *info, VkFormat format)
{
   return fd6_ubwc_compat_mode(info, vk_format_to_pipe_format(format));
}

bool
tu6_mutable_format_list_ubwc_compatible(const struct fd_dev_info *info,
                                        const VkImageFormatListCreateInfo *fmt_list)
{
   if (!fmt_list || !fmt_list->viewFormatCount)
      return false;

   if (fmt_list->viewFormatCount == 1)
      return true;

   enum fd6_ubwc_compat_type type =
      tu6_ubwc_compat_mode(info, fmt_list->pViewFormats[0]);
   if (type == FD6_UBWC_UNKNOWN_COMPAT)
      return false;

   for (uint32_t i = 1; i < fmt_list->viewFormatCount; i++) {
      if (tu6_ubwc_compat_mode(info, fmt_list->pViewFormats[i]) != type)
         return false;
   }

   return true;
}

static bool
tu_format_linear_filtering_supported(struct tu_physical_device *physical_device,
                                     VkFormat vk_format)
{
   if (physical_device->info->props.is_a702) {
      switch (vk_format) {
      case VK_FORMAT_D16_UNORM:
      case VK_FORMAT_D24_UNORM_S8_UINT:
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D32_SFLOAT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT:
      case VK_FORMAT_R16_UNORM:
      case VK_FORMAT_R16_SNORM:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16_SNORM:
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
         return false;
      }
   }
   return !vk_format_is_int(vk_format);
}

static void
tu_physical_device_get_format_properties(
   struct tu_physical_device *physical_device,
   VkFormat vk_format,
   VkFormatProperties3 *out_properties,
   VkSubpassResolvePerformanceQueryEXT *msrtss_out)
{
   VkFormatFeatureFlags2 linear = 0, optimal = 0, buffer = 0;
   enum pipe_format format = vk_format_to_pipe_format(vk_format);
   const struct util_format_description *desc = util_format_description(format);
   const struct vk_format_ycbcr_info *ycbcr_info = vk_format_get_ycbcr_info(vk_format);

   bool supported_vtx = tu6_format_vtx_supported(format);
   bool supported_color = tu6_format_color_supported(physical_device->info, format);
   bool supported_tex = fd6_texture_format_supported(physical_device->info, format,
                                                     TILE6_LINEAR, false);
   bool is_npot = !util_is_power_of_two_or_zero(desc->block.bits);

   if (format == PIPE_FORMAT_NONE ||
       !(supported_vtx || supported_color || supported_tex)) {
      goto end;
   }

   if (msrtss_out)
      msrtss_out->optimal = true;

   if (!is_npot)
      buffer |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;

   if (supported_vtx)
      buffer |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;

   if (supported_tex)
      buffer |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

   if (!is_npot && !util_format_has_depth(util_format_description(format))) {
      optimal |= VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT;
   }

   if (supported_tex && !is_npot) {
      optimal |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                 VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
                 VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                 VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT;

      if (ycbcr_info) {
         optimal |= VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT;

         if (ycbcr_info->n_planes > 1) {
            optimal |= VK_FORMAT_FEATURE_2_COSITED_CHROMA_SAMPLES_BIT |
                       VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
            if (physical_device->info->props.has_separate_chroma_filter)
               optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT;
         }
      } else {
         optimal |= VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;
      }

      if (tu_format_linear_filtering_supported(physical_device, vk_format)) {
         optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

         if (physical_device->vk.supported_extensions.EXT_filter_cubic)
            optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT;
      }

      if (vk_format_is_float(vk_format) && desc->nr_channels == 2 &&
          desc->swizzle[0] == PIPE_SWIZZLE_X &&
          desc->swizzle[1] == PIPE_SWIZZLE_Y) {
         optimal |= VK_FORMAT_FEATURE_2_FRAGMENT_DENSITY_MAP_BIT_EXT;
      }
   }

   if (supported_color) {
      assert(supported_tex);
      optimal |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_2_BLIT_DST_BIT |
                 VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                 VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                 VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

      buffer |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
                VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

      if (vk_format == VK_FORMAT_R32_UINT || vk_format == VK_FORMAT_R32_SINT ||
          vk_format == VK_FORMAT_R32_SFLOAT) {
         optimal |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT;
         buffer |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
      }

      if (!vk_format_is_int(vk_format))
         optimal |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
   }

   if (vk_format_has_depth(vk_format) && (optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
      optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
   }

   if (desc->nr_channels > 2 && desc->block.bits == 16) {
      buffer &= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
      optimal &= ~(VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                   VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT);
   }

   if ((optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) &&
       (!ycbcr_info || ycbcr_info->n_planes == 1) &&
       !vk_format_is_depth_or_stencil(vk_format)) {
      int c = util_format_get_first_non_void_channel(desc->format);
      bool is_8bpc = c != -1 && desc->is_array && desc->channel[c].size == 8;

      if ((is_8bpc && vk_format != VK_FORMAT_B8G8R8A8_UNORM &&
           vk_format != VK_FORMAT_B8G8R8A8_SNORM &&
           vk_format != VK_FORMAT_B8G8R8A8_SRGB) ||
          vk_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
         if (desc->is_unorm &&
             desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB)
            optimal |= VK_FORMAT_FEATURE_2_BLOCK_MATCHING_BIT_QCOM;
         if ((desc->is_unorm || desc->is_snorm) &&
             vk_format != VK_FORMAT_R8G8_SNORM) {
            optimal |= VK_FORMAT_FEATURE_2_BOX_FILTER_SAMPLED_BIT_QCOM;
            optimal |= VK_FORMAT_FEATURE_2_WEIGHT_SAMPLED_IMAGE_BIT_QCOM;
         }
      }

      if (vk_format == VK_FORMAT_B5G6R5_UNORM_PACK16 ||
          vk_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
          vk_format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ||
          util_format_is_float16(format) ||
          (util_format_is_compressed(format) &&
           desc->layout != UTIL_FORMAT_LAYOUT_RGTC &&
           vk_format != VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK &&
           vk_format != VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK &&
           vk_format != VK_FORMAT_EAC_R11G11_UNORM_BLOCK &&
           vk_format != VK_FORMAT_EAC_R11G11_SNORM_BLOCK)) {
         optimal |= VK_FORMAT_FEATURE_2_BOX_FILTER_SAMPLED_BIT_QCOM;
         optimal |= VK_FORMAT_FEATURE_2_WEIGHT_SAMPLED_IMAGE_BIT_QCOM;
      }

      if (vk_format == VK_FORMAT_R8_UNORM ||
          vk_format == VK_FORMAT_R16_SFLOAT)
         optimal |= VK_FORMAT_FEATURE_2_WEIGHT_IMAGE_BIT_QCOM;
   }

   linear = optimal;
   if (tu6_pipe2depth(vk_format) != DEPTH6_NONE)
      optimal |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;

   if (!tiling_possible(vk_format) &&
       vk_format != VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM) {
      optimal = 0;
   }

   if (ycbcr_info || vk_format_is_depth_or_stencil(vk_format))
      buffer = 0;

   if (vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT)
      linear = 0;

   if (vk_format == VK_FORMAT_R8_UINT)
      optimal |= VK_FORMAT_FEATURE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;

   if (vk_acceleration_struct_vtx_format_supported(vk_format))
      buffer |= VK_FORMAT_FEATURE_2_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;

end:
   out_properties->linearTilingFeatures = linear;
   out_properties->optimalTilingFeatures = optimal;
   out_properties->bufferFeatures = buffer;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceFormatProperties2(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);

   VkFormatProperties3 local_props3;
   VkFormatProperties3 *props3 =
      vk_find_struct(pFormatProperties->pNext, FORMAT_PROPERTIES_3);
   if (!props3)
      props3 = &local_props3;
   VkSubpassResolvePerformanceQueryEXT *msrtss_out =
      vk_find_struct(pFormatProperties->pNext,
                     SUBPASS_RESOLVE_PERFORMANCE_QUERY_EXT);

   tu_physical_device_get_format_properties(
      physical_device, format, props3, msrtss_out);

   pFormatProperties->formatProperties = (VkFormatProperties) {
      .linearTilingFeatures =
         vk_format_features2_to_features(props3->linearTilingFeatures),
      .optimalTilingFeatures =
         vk_format_features2_to_features(props3->optimalTilingFeatures),
      .bufferFeatures =
         vk_format_features2_to_features(props3->bufferFeatures),
   };

   VkDrmFormatModifierPropertiesListEXT *list =
      vk_find_struct(pFormatProperties->pNext, DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
   if (list) {
      VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierPropertiesEXT, out,
                             list->pDrmFormatModifierProperties,
                             &list->drmFormatModifierCount);

      if (pFormatProperties->formatProperties.linearTilingFeatures) {
         vk_outarray_append_typed(VkDrmFormatModifierPropertiesEXT, &out, mod_props) {
            mod_props->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
            mod_props->drmFormatModifierPlaneCount = tu6_plane_count(format);
            mod_props->drmFormatModifierTilingFeatures =
               pFormatProperties->formatProperties.linearTilingFeatures;
         }
      }

      if (pFormatProperties->formatProperties.optimalTilingFeatures &&
          tiling_possible(format) &&
          ubwc_possible(NULL, format, VK_IMAGE_TYPE_2D, 0, 0, 0,
                        physical_device->info, VK_SAMPLE_COUNT_1_BIT, 1,
                        false)) {
         vk_outarray_append_typed(VkDrmFormatModifierPropertiesEXT, &out, mod_props) {
            mod_props->drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
            mod_props->drmFormatModifierPlaneCount = tu6_plane_count(format);
            mod_props->drmFormatModifierTilingFeatures =
               pFormatProperties->formatProperties.optimalTilingFeatures;
         }
      }
   }

   VkDrmFormatModifierPropertiesList2EXT *list2 =
      vk_find_struct(pFormatProperties->pNext, DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT);
   if (list2) {
      VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierProperties2EXT, out,
                             list2->pDrmFormatModifierProperties,
                             &list2->drmFormatModifierCount);

      if (props3->linearTilingFeatures) {
         vk_outarray_append_typed(VkDrmFormatModifierProperties2EXT, &out, mod_props) {
            mod_props->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
            mod_props->drmFormatModifierPlaneCount = tu6_plane_count(format);
            mod_props->drmFormatModifierTilingFeatures =
               props3->linearTilingFeatures;
         }
      }

      if (props3->optimalTilingFeatures &&
          tiling_possible(format) &&
          ubwc_possible(NULL, format, VK_IMAGE_TYPE_2D, 0, 0, 0,
                        physical_device->info, VK_SAMPLE_COUNT_1_BIT, 1,
                        false)) {
         vk_outarray_append_typed(VkDrmFormatModifierProperties2EXT, &out, mod_props) {
            mod_props->drmFormatModifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
            mod_props->drmFormatModifierPlaneCount = tu6_plane_count(format);
            mod_props->drmFormatModifierTilingFeatures =
               props3->optimalTilingFeatures;
         }
      }
   }
}

static VkResult
tu_image_unsupported_format(VkImageFormatProperties *pImageFormatProperties)
{
   *pImageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = { 0, 0, 0 },
      .maxMipLevels = 0,
      .maxArrayLayers = 0,
      .sampleCounts = 0,
      .maxResourceSize = 0,
   };

   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static VkResult
tu_get_image_format_properties(
   struct tu_physical_device *physical_device,
   const VkPhysicalDeviceImageFormatInfo2 *info,
   VkImageFormatProperties *pImageFormatProperties,
   VkFormatFeatureFlags *p_feature_flags)
{
   VkFormatProperties3 format_props;
   VkFormatFeatureFlags format_feature_flags;
   VkExtent3D maxExtent;
   uint32_t maxMipLevels;
   uint32_t maxArraySize;
   BITMASK_ENUM(VkSampleCountFlagBits) sampleCounts = VK_SAMPLE_COUNT_1_BIT;

   tu_physical_device_get_format_properties(physical_device, info->format,
                                            &format_props, NULL);

   switch (info->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      format_feature_flags = format_props.linearTilingFeatures;
      break;

   case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: {
      const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *drm_info =
         vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);

      if (info->flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      if (info->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      switch (drm_info->drmFormatModifier) {
      case DRM_FORMAT_MOD_QCOM_COMPRESSED:
         if (!format_props.optimalTilingFeatures ||
             !tiling_possible(info->format))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;

         if (info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
            const VkImageFormatListCreateInfo *format_list =
               vk_find_struct_const(info->pNext,
                                    IMAGE_FORMAT_LIST_CREATE_INFO);
            if (!tu6_mutable_format_list_ubwc_compatible(physical_device->info,
                                                         format_list))
               return VK_ERROR_FORMAT_NOT_SUPPORTED;
         }

         if (!ubwc_possible(NULL, info->format, info->type, info->flags,
                            info->usage, info->usage, physical_device->info,
                            sampleCounts, 1, false)) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
         }

         format_feature_flags = format_props.optimalTilingFeatures;
         break;
      case DRM_FORMAT_MOD_LINEAR:
         format_feature_flags = format_props.linearTilingFeatures;
         break;
      default:
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
   } break;
   case VK_IMAGE_TILING_OPTIMAL:
      format_feature_flags = format_props.optimalTilingFeatures;
      break;
   default:
      UNREACHABLE("bad VkPhysicalDeviceImageFormatInfo2");
   }

   if (format_feature_flags == 0)
      return tu_image_unsupported_format(pImageFormatProperties);

   if (info->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
      if (!physical_device->has_sparse)
         return tu_image_unsupported_format(pImageFormatProperties);
   }

   if (info->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) {
      if (vk_format_get_plane_count(info->format) > 1)
         return tu_image_unsupported_format(pImageFormatProperties);

      if (info->usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT)
         return tu_image_unsupported_format(pImageFormatProperties);

      if (info->type != VK_IMAGE_TYPE_2D ||
          info->tiling != VK_IMAGE_TILING_OPTIMAL ||
          !tiling_possible(info->format) ||
          (info->usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT))
         return tu_image_unsupported_format(pImageFormatProperties);
   }

   if (info->type != VK_IMAGE_TYPE_2D &&
       vk_format_is_depth_or_stencil(info->format))
      return tu_image_unsupported_format(pImageFormatProperties);

   switch (info->type) {
   default:
      UNREACHABLE("bad vkimage type\n");
   case VK_IMAGE_TYPE_1D:
      maxExtent.width = 16384;
      maxExtent.height = 1;
      maxExtent.depth = 1;
      maxMipLevels = 15;
      maxArraySize = 2048;
      break;
   case VK_IMAGE_TYPE_2D:
      maxExtent.width = 16384;
      maxExtent.height = 16384;
      maxExtent.depth = 1;
      maxMipLevels = 15;
      maxArraySize = 2048;
      break;
   case VK_IMAGE_TYPE_3D:
      maxExtent.width = 2048;
      maxExtent.height = 2048;
      maxExtent.depth = 2048;
      maxMipLevels = 12;
      maxArraySize = 1;
      break;
   }

   if (info->tiling == VK_IMAGE_TILING_OPTIMAL &&
       info->type == VK_IMAGE_TYPE_2D &&
       (format_feature_flags &
        (VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       !(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       !(info->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
      sampleCounts |= VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;

      if (physical_device->info->chip >= A7XX &&
          vk_format_get_blocksizebits(info->format) <= 64)
         sampleCounts |= VK_SAMPLE_COUNT_8_BIT;
   }

   VkImageUsageFlags image_usage = info->usage;
   if (info->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
      image_usage = 0;

   if (image_usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   if (image_usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   if (image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   if (image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      if (!(format_feature_flags &
            VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   if (image_usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) {
      if (!(format_feature_flags &
            (VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
             VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   if (image_usage &
       VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) {
      if (!(format_feature_flags &
            VK_FORMAT_FEATURE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
         return tu_image_unsupported_format(pImageFormatProperties);
      }
   }

   *pImageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = maxExtent,
      .maxMipLevels = maxMipLevels,
      .maxArrayLayers = maxArraySize,
      .sampleCounts = sampleCounts,
      .maxResourceSize = UINT32_MAX,
   };

   if (p_feature_flags)
      *p_feature_flags = format_feature_flags;

   return VK_SUCCESS;
}

static VkResult
tu_get_external_image_format_properties(
   const struct tu_physical_device *physical_device,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkExternalMemoryHandleTypeFlagBits handleType,
   VkExternalImageFormatProperties *external_properties)
{
   BITMASK_ENUM(VkExternalMemoryFeatureFlagBits) flags = 0;
   VkExternalMemoryHandleTypeFlags export_flags = 0;
   VkExternalMemoryHandleTypeFlags compat_flags = 0;

   assert(handleType !=
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      switch (pImageFormatInfo->type) {
      case VK_IMAGE_TYPE_2D:
         flags = VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT |
                 VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                 VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
         compat_flags = export_flags =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         break;
      default:
         return vk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "VkExternalMemoryTypeFlagBits(0x%x) unsupported for VkImageType(%d)",
                          handleType, pImageFormatInfo->type);
      }
      break;
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
      flags = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      compat_flags = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
      break;
   default:
      return vk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "VkExternalMemoryTypeFlagBits(0x%x) unsupported",
                       handleType);
   }

   if (external_properties) {
      external_properties->externalMemoryProperties =
         (VkExternalMemoryProperties) {
            .externalMemoryFeatures = flags,
            .exportFromImportedHandleTypes = export_flags,
            .compatibleHandleTypes = compat_flags,
         };
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *base_info,
   VkImageFormatProperties2 *base_props)
{
   VK_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);
   const VkPhysicalDeviceExternalImageFormatInfo *external_info = NULL;
   const VkPhysicalDeviceImageViewImageFormatInfoEXT *image_view_info = NULL;
   VkExternalImageFormatProperties *external_props = NULL;
   VkFilterCubicImageViewImageFormatPropertiesEXT *cubic_props = NULL;
   VkFormatFeatureFlags format_feature_flags;
   VkSamplerYcbcrConversionImageFormatProperties *ycbcr_props = NULL;
   VkHostImageCopyDevicePerformanceQueryEXT *hic_props = NULL;
   VkResult result;

   result = tu_get_image_format_properties(physical_device,
      base_info, &base_props->imageFormatProperties, &format_feature_flags);
   if (result != VK_SUCCESS)
      return result;

   vk_foreach_struct_const(s, base_info->pNext)
   {
      switch (s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
         external_info = (const VkPhysicalDeviceExternalImageFormatInfo *) s;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT:
         image_view_info = (const VkPhysicalDeviceImageViewImageFormatInfoEXT *) s;
         break;
      default:
         break;
      }
   }

   vk_foreach_struct(s, base_props->pNext)
   {
      switch (s->sType) {
      case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
         external_props = (VkExternalImageFormatProperties *) s;
         break;
      case VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT:
         cubic_props = (VkFilterCubicImageViewImageFormatPropertiesEXT *) s;
         break;
      case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES:
         ycbcr_props = (VkSamplerYcbcrConversionImageFormatProperties *) s;
         break;
      case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY_EXT:
         hic_props = (VkHostImageCopyDevicePerformanceQueryEXT *) s;
         break;
      default:
         break;
      }
   }

   if (external_info && external_info->handleType != 0) {
      if (external_info->handleType ==
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
         result = vk_android_get_ahb_image_properties(physicalDevice,
                                                      base_info, base_props);
         if (result != VK_SUCCESS)
            goto fail;

         VkImageFormatProperties *props = &base_props->imageFormatProperties;
         if (!(props->sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
            result = vk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                               "sampleCounts (%x) unsupported for AHB",
                               props->sampleCounts);
            goto fail;
         }

         props->maxMipLevels = 1;
         props->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
      } else {
         result = tu_get_external_image_format_properties(
            physical_device, base_info, external_info->handleType,
            external_props);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   if (cubic_props) {
      if ((image_view_info->imageViewType == VK_IMAGE_VIEW_TYPE_2D ||
           image_view_info->imageViewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) &&
          (format_feature_flags & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT)) {
         cubic_props->filterCubic = true;
         cubic_props->filterCubicMinmax = true;
      } else {
         cubic_props->filterCubic = false;
         cubic_props->filterCubicMinmax = false;
      }
   }

   if (ycbcr_props)
      ycbcr_props->combinedImageSamplerDescriptorCount = 1;

   if (hic_props) {
      hic_props->optimalDeviceAccess = hic_props->identicalMemoryLayout =
         base_info->tiling == VK_IMAGE_TILING_LINEAR ||
         base_info->type == VK_IMAGE_TYPE_1D ||
         !tiling_possible(base_info->format) ||
         (base_info->usage & VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT) ||
         (fd6_color_swap(vk_format_to_pipe_format(base_info->format),
                                                  TILE6_LINEAR, false) == WZYX &&
         !ubwc_possible(NULL, base_info->format, base_info->type,
                        base_info->flags,
                        (base_info->usage & ~VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT),
                        (base_info->usage & ~VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT),
                        physical_device->info, VK_SAMPLE_COUNT_1_BIT, 1,
                        physical_device->info->props.has_z24uint_s8uint));
   }

   return VK_SUCCESS;

fail:
   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
      base_props->imageFormatProperties = (VkImageFormatProperties) {};
   }

   return result;
}