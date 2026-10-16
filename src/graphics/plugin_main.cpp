/**
 * @file        graphics/plugin_main.cpp
 * @brief       rexgpu-xenos plugin entry points
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string_view>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#if REX_HAS_D3D12 && REX_HAS_VULKAN
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#endif

#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

#if REX_HAS_D3D12 && REX_HAS_VULKAN
namespace {
// Whether the GPU Direct3D 12 would run on is an AMD integrated (UMA) part.
// Only the default adapter choice is checked: a player who set d3d12_adapter
// picked a card on purpose. The dedicated-memory figure cannot tell (an APU
// reported 4 GB in #36), so a throwaway device is asked for UMA directly.
bool IsAmdIntegratedGpu() {
  if (REXCVAR_GET(d3d12_adapter) != -1) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(
           i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
       ++i) {
    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      continue;
    }
    if (desc.VendorId != 0x1002) {
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
      return false;
    }
    D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch,
                                                 sizeof(arch))) &&
           arch.UMA;
  }
  return false;
}
}  // namespace
#endif

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-xenos: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xenos: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
  // A config can carry a value no build understands (seen in the wild: the
  // plugin's own name, 'xenos'). Refusing it leaves the game unable to start
  // at all, so anything that is not a known backend means "any".
  if (backend != "any" && backend != "d3d12" && backend != "vulkan") {
    REXLOG_WARN("rexgpu-xenos: unknown backend '{}', using the default", backend);
    backend = "any";
  }
#if REX_HAS_D3D12 && REX_HAS_VULKAN
  // AMD integrated Radeon hangs the GPU under our Direct3D 12 renderer
  // (DEVICE_HUNG at startup, #41) and runs fine on Vulkan, so Automatic
  // picks Vulkan there. An explicit "d3d12" is still honoured.
  if (backend == "any" && IsAmdIntegratedGpu()) {
    REXLOG_INFO("rexgpu-xenos: AMD integrated graphics, Automatic picks Vulkan");
    backend = "vulkan";
  }
#endif
#if REX_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    return new rex::graphics::d3d12::D3D12GraphicsSystem();
  }
#endif
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    return new rex::graphics::vulkan::VulkanGraphicsSystem();
  }
#endif
  REXLOG_ERROR("rexgpu-xenos: requested backend '{}' is not compiled into this plugin", backend);
  return nullptr;
}
