/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/immediate_drawer.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(vulkan_validation_enabled, false, "UI/Vulkan",
                    "Enable Vulkan validation layers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_device, -1, "UI/Vulkan", "Vulkan device index (-1 for auto selection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_prefer_geometry_shader, true, "UI/Vulkan",
                    "Prefer physical devices supporting geometryShader when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_prefer_fragment_stores_and_atomics, true, "UI/Vulkan",
    "Prefer physical devices supporting fragmentStoresAndAtomics when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_vertex_pipeline_stores_and_atomics, true, "UI/Vulkan",
                    "Prefer physical devices supporting vertexPipelineStoresAndAtomics when "
                    "auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_fill_mode_non_solid, true, "UI/Vulkan",
                    "Prefer physical devices supporting fillModeNonSolid when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_discrete_gpu, true, "UI/Vulkan",
                    "Prefer a discrete GPU over an integrated one when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

// Ranks a physical device for auto-selection: higher wins. Laptops and APU
// desktops enumerate the integrated GPU first as often as not, and every
// feature the preference cvars above look at is supported by both, so without
// this the game happily starts on the iGPU - the single most common cause of a
// bad first-run frame rate.
uint32_t PhysicalDeviceTypeRank(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
      return 1;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
    default:
      return 0;
  }
}

const char* PhysicalDeviceTypeName(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "cpu";
    default:
      return "other";
  }
}

}  // namespace


std::unique_ptr<VulkanProvider> VulkanProvider::Create(const bool with_gpu_emulation,
                                                       const bool with_presentation) {
  std::unique_ptr<VulkanProvider> provider(new VulkanProvider());

  provider->vulkan_instance_ =
      VulkanInstance::Create(with_presentation, REXCVAR_GET(vulkan_validation_enabled));
  if (!provider->vulkan_instance_) {
    return nullptr;
  }

  std::vector<VkPhysicalDevice> physical_devices;
  provider->vulkan_instance_->EnumeratePhysicalDevices(physical_devices);

  if (physical_devices.empty()) {
    REXLOG_WARN("No Vulkan physical devices available");
    return nullptr;
  }

  const VulkanInstance::Functions& ifn = provider->vulkan_instance_->functions();

  REXLOG_WARN(
      "Available Vulkan physical devices (use the 'vulkan_device' "
      "configuration variable to force a specific device):");
  for (size_t physical_device_index = 0; physical_device_index < physical_devices.size();
       ++physical_device_index) {
    VkPhysicalDeviceProperties physical_device_properties;
    ifn.vkGetPhysicalDeviceProperties(physical_devices[physical_device_index],
                                      &physical_device_properties);
    REXLOG_WARN("* {}: {} ({})", physical_device_index, physical_device_properties.deviceName,
                PhysicalDeviceTypeName(physical_device_properties.deviceType));
  }

  if (REXCVAR_GET(vulkan_device) >= 0 &&
      uint32_t(REXCVAR_GET(vulkan_device)) < physical_devices.size()) {
    provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
        provider->vulkan_instance_.get(), physical_devices[REXCVAR_GET(vulkan_device)],
        with_gpu_emulation, with_presentation);
  }

  if (!provider->vulkan_device_) {
    std::vector<VkPhysicalDevice> physical_devices_ordered = physical_devices;
    bool prefer_geometry_shader = REXCVAR_GET(vulkan_prefer_geometry_shader);
    bool prefer_fragment_stores = REXCVAR_GET(vulkan_prefer_fragment_stores_and_atomics);
    bool prefer_vertex_stores = REXCVAR_GET(vulkan_prefer_vertex_pipeline_stores_and_atomics);
    bool prefer_fill_mode_non_solid = REXCVAR_GET(vulkan_prefer_fill_mode_non_solid);
    bool prefer_discrete_gpu = REXCVAR_GET(vulkan_prefer_discrete_gpu);
    if (with_gpu_emulation && physical_devices.size() > 1 &&
        (prefer_discrete_gpu || prefer_geometry_shader || prefer_fragment_stores ||
         prefer_vertex_stores || prefer_fill_mode_non_solid)) {
      struct PhysicalDeviceScore {
        VkPhysicalDevice physical_device;
        // Ordered lexicographically: the device type outranks every feature
        // preference, because an integrated GPU that ticks all four feature
        // boxes is still the wrong device to run the game on.
        uint32_t type_rank;
        uint32_t feature_score;
      };
      std::vector<PhysicalDeviceScore> scored_devices;
      scored_devices.reserve(physical_devices.size());
      for (const VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceFeatures supported_features = {};
        ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
        uint32_t feature_score = 0;
        if (prefer_geometry_shader && supported_features.geometryShader) {
          ++feature_score;
        }
        if (prefer_fragment_stores && supported_features.fragmentStoresAndAtomics) {
          ++feature_score;
        }
        if (prefer_vertex_stores && supported_features.vertexPipelineStoresAndAtomics) {
          ++feature_score;
        }
        if (prefer_fill_mode_non_solid && supported_features.fillModeNonSolid) {
          ++feature_score;
        }
        uint32_t type_rank = 0;
        if (prefer_discrete_gpu) {
          VkPhysicalDeviceProperties properties;
          ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
          type_rank = PhysicalDeviceTypeRank(properties.deviceType);
        }
        scored_devices.push_back({physical_device, type_rank, feature_score});
      }

      std::stable_sort(scored_devices.begin(), scored_devices.end(),
                       [](const PhysicalDeviceScore& a, const PhysicalDeviceScore& b) {
                         if (a.type_rank != b.type_rank) {
                           return a.type_rank > b.type_rank;
                         }
                         return a.feature_score > b.feature_score;
                       });

      const bool order_differs =
          !scored_devices.empty() &&
          (scored_devices.front().type_rank != scored_devices.back().type_rank ||
           scored_devices.front().feature_score != scored_devices.back().feature_score);
      if (order_differs) {
        physical_devices_ordered.clear();
        physical_devices_ordered.reserve(scored_devices.size());
        for (const PhysicalDeviceScore& scored_device : scored_devices) {
          physical_devices_ordered.push_back(scored_device.physical_device);
        }
      }
    }

    for (const VkPhysicalDevice physical_device : physical_devices_ordered) {
      provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
          provider->vulkan_instance_.get(), physical_device, with_gpu_emulation, with_presentation);
      if (provider->vulkan_device_) {
        break;
      }
    }

    if (!provider->vulkan_device_) {
      REXLOG_WARN(
          "Couldn't choose a compatible Vulkan physical device or initialize a "
          "Vulkan logical device");
      return nullptr;
    }
  }

  {
    // Say out loud which device won. When a player reports a bad frame rate,
    // this one line answers "is it running on the iGPU?" without a back and
    // forth.
    VkPhysicalDeviceProperties chosen_properties;
    ifn.vkGetPhysicalDeviceProperties(provider->vulkan_device_->physical_device(),
                                      &chosen_properties);
    REXLOG_INFO("Vulkan device selected: {} ({})", chosen_properties.deviceName,
                PhysicalDeviceTypeName(chosen_properties.deviceType));
    if (chosen_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        physical_devices.size() > 1) {
      REXLOG_WARN(
          "Running on an integrated GPU with other devices present - expect a poor "
          "frame rate. Set the 'vulkan_device' configuration variable to the index "
          "of your dedicated GPU listed above, or force it in the driver control panel.");
    }
  }

  if (with_presentation) {
    provider->ui_samplers_ = UISamplers::Create(provider->vulkan_device_.get());
    if (!provider->ui_samplers_) {
      return nullptr;
    }
  }

  return provider;
}

std::unique_ptr<Presenter> VulkanProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return VulkanPresenter::Create(host_gpu_loss_callback, vulkan_device(), ui_samplers());
}

std::unique_ptr<ImmediateDrawer> VulkanProvider::CreateImmediateDrawer() {
  return VulkanImmediateDrawer::Create(vulkan_device(), ui_samplers());
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
