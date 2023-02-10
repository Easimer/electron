// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#define SK_API
#include "skia/config/SkUserConfig.h"

#include "base/bind.h"
#include "base/logging.h"
#include "build/build_config.h"
#include "electron/native_api/electron.h"
#include "electron/native_api/offscreen.h"
#include "native_api/egl/context.h"
#include "native_api/egl/thread_state.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/gfx/gpu_fence.h"

#include <vector>

#if BUILDFLAG(IS_WIN)
#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#endif

namespace electron {
namespace api {
namespace gpu {

struct FenceSet {
  size_t num_fences;

  egl::Context* context;
  /** Buffer for GLES fences */
  std::vector<uint32_t> fence_ids;
  /** Buffer for GPU fences */
  std::vector<std::unique_ptr<gfx::GpuFence>> fences;

  size_t idx_cur_fence;

#if BUILDFLAG(IS_WIN)
  /** The secondary context's device */
  ID3D11Device5* device;
  /** The secondary context */
  ID3D11DeviceContext4* device_context;
  /** Buffer for D3D11 fences */
  std::vector<Microsoft::WRL::ComPtr<ID3D11Fence>> d3d11_fences;
#endif
};

static std::vector<absl::optional<FenceSet>> fence_set_pool;

#if BUILDFLAG(IS_WIN)
static bool DeviceWait(FenceSet& set, size_t idx_fence) {
  const gfx::GpuFenceHandle& handle =
      set.fences[idx_fence]->GetGpuFenceHandle();

  HANDLE hFence = handle.owned_handle.get();

  // Create a fence using the shared handle and Wait on it with a completion
  // value of 1 (the GPU process has already called Signal(1) on it at creation
  // time)

  Microsoft::WRL::ComPtr<ID3D11Fence> d3d11_fence;
  HRESULT hr;
  hr = set.device->OpenSharedFence(hFence, IID_PPV_ARGS(&d3d11_fence));
  if (FAILED(hr)) {
    LOG(ERROR) << "Unable to open shared ID3D11Fence "
               << logging::SystemErrorCodeToString(hr);
    return false;
  }

  hr = set.device_context->Wait(d3d11_fence.Get(), 1);
  if (FAILED(hr)) {
    LOG(ERROR) << "Unable to wait on ID3D11Fence "
               << logging::SystemErrorCodeToString(hr);
    return false;
  }

  // We must not release the fence until this frame comes up again
  set.d3d11_fences[idx_fence] = std::move(d3d11_fence);

  return true;
}
#else
static bool DeviceWait(FenceSet&, size_t) {
  LOG(ERROR) << "DeviceWait stub";
  return false;
}
#endif

ELECTRON_EXTERN bool createFenceSet(FenceSetHandle* out_handle,
                                    const FenceSetCreateInfo* create_info) {
  if (out_handle == nullptr || create_info == nullptr) {
    return false;
  }

  if (create_info->device == nullptr) {
    return false;
  }

  FenceSetDeviceType device_type =
      reinterpret_cast<const FenceSetDeviceBase* const>(create_info->device)
          ->type;

#if BUILDFLAG(IS_WIN)
  if (device_type != FENCE_SET_DEVICE_TYPE_D3D11) {
    DLOG(ERROR) << "Unexpected device type";
    return false;
  }

  auto* device_info =
      reinterpret_cast<const FenceSetDeviceD3D11* const>(create_info->device);
#else
  return false;
#endif

  egl::ThreadState* ts = egl::ThreadState::Get();
  egl::Context* context = ts->current_context();

  if (context == nullptr) {
    LOG(ERROR) << "Unable to get current EGL context";
    return false;
  }

  // Find an empty slot for the fence set

  absl::optional<FenceSet>* dst_slot = nullptr;
  uintptr_t idx_slot = 0;

  for (uintptr_t i = 0; i < fence_set_pool.size(); i++) {
    if (!fence_set_pool[i].has_value()) {
      dst_slot = &fence_set_pool[i];
      idx_slot = i;
      break;
    }
  }

  // Allocate a new slot for the fence set

  if (dst_slot == nullptr) {
    idx_slot = fence_set_pool.size();
    fence_set_pool.push_back({});
    dst_slot = &fence_set_pool.back();
  }

  FenceSet set;
  set.num_fences = create_info->num_fences;
  set.context = context;
  set.idx_cur_fence = 0;

#if BUILDFLAG(IS_WIN)
  set.device = (ID3D11Device5*)device_info->device;
  set.device_context = (ID3D11DeviceContext4*)device_info->device_context;
  set.d3d11_fences.resize(set.num_fences);
#endif

  set.fence_ids.resize(set.num_fences);
  set.fences.resize(set.num_fences);

  *dst_slot = std::move(set);
  *out_handle = reinterpret_cast<FenceSetHandle>(idx_slot);

  return true;
}

ELECTRON_EXTERN bool destroyFenceSet(FenceSetHandle handle) {
  uintptr_t idx_slot = reinterpret_cast<uintptr_t>(handle);
  if (idx_slot >= fence_set_pool.size() ||
      !fence_set_pool[idx_slot].has_value()) {
    return false;
  }

  FenceSet set = std::move(fence_set_pool[idx_slot].value());

#if BUILDFLAG(IS_WIN)
  set.d3d11_fences.clear();
#endif

  egl::Context* context = set.context;
  context->DestroyGpuFences(set.num_fences, set.fence_ids.data(),
                            set.fences.data());

  fence_set_pool[idx_slot].reset();

  // Shrink the pool if this set was the last slot
  if (idx_slot == fence_set_pool.size() - 1) {
    fence_set_pool.pop_back();
  }

  return true;
}

ELECTRON_EXTERN bool insertDependency(FenceSetHandle handle) {
  uintptr_t idx_slot = reinterpret_cast<uintptr_t>(handle);
  if (idx_slot >= fence_set_pool.size() ||
      !fence_set_pool[idx_slot].has_value()) {
    return false;
  }

  FenceSet& set = fence_set_pool[idx_slot].value();
  egl::Context* context = set.context;

  size_t idx_fence = set.idx_cur_fence;
  set.idx_cur_fence = (set.idx_cur_fence + 1) % set.num_fences;

  if (set.fences[idx_fence] != nullptr) {
#if BUILDFLAG(IS_WIN)
    set.d3d11_fences[idx_fence].Reset();
#endif
    context->DestroyGpuFences(1, &set.fence_ids[idx_fence],
                              &set.fences[idx_fence]);
  }

  if (!context->CreateGpuFences(1, &set.fence_ids[idx_fence],
                                &set.fences[idx_fence])) {
    return false;
  }

  return DeviceWait(set, idx_fence);
}

}  // namespace gpu
}  // namespace api
}  // namespace electron
