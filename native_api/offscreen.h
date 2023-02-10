// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef NATIVE_API_OFFSCREEN
#define NATIVE_API_OFFSCREEN

#include <functional>
#include <map>

#include "electron/native_api/electron.h"

namespace gpu {
  struct Mailbox;
  struct SyncToken;
}

namespace electron {
namespace api {
namespace gpu {

enum CommandBufferNamespace : int8_t {
  INVALID = -1,

  GPU_IO,
  IN_PROCESS,
  VIZ_SKIA_OUTPUT_SURFACE,
  VIZ_SKIA_OUTPUT_SURFACE_NON_DDL,

  NUM_COMMAND_BUFFER_NAMESPACES
};

struct SyncToken {
  bool verified_flush;
  CommandBufferNamespace namespace_id;
  uint64_t command_buffer_id;
  uint64_t release_count;

  int8_t* GetData() { return reinterpret_cast<int8_t*>(this); }

  const int8_t* GetConstData() const {
    return reinterpret_cast<const int8_t*>(this);
  }
};

struct Mailbox {
  int8_t name[16];
  bool shared_image;

  bool operator<(const Mailbox& other) const {
    return memcmp(this, &other, sizeof other) < 0;
  }
  bool operator==(const Mailbox& other) const {
    return memcmp(this, &other, sizeof other) == 0;
  }
  bool operator!=(const Mailbox& other) const { return !operator==(other); }
};

/**
 * Handle to a fence set.
 */
using FenceSetHandle = struct FenceSetHandle_T*;

/**
 * Values used to specify which graphics API to create the fence set for.
 */
enum FenceSetDeviceType {
  FENCE_SET_DEVICE_TYPE_INVALID = 0,
  FENCE_SET_DEVICE_TYPE_D3D11,
  FENCE_SET_DEVICE_TYPE_MAX
};

struct FenceSetDeviceBase {
  /**
   * Which graphics API to create the fence set for.
   */
  FenceSetDeviceType type;
};

struct FenceSetDeviceD3D11 {
  /**
   * Which graphics API to create the fence set for. Must be
   * FENCE_SET_DEVICE_TYPE_D3D11.
   */
  FenceSetDeviceType type;

  /**
   * A valid pointer to a ID3D11Device5 object.
   */
  void* device;
  /**
   * A valid pointer to a ID3D11DeviceContext4 object.
   */
  void* device_context;
};

/**
 * A structure containing information about how the fence set is to be created.
 */
struct FenceSetCreateInfo {
  /**
   * The number of fences that can be in-flight simultaneously.
   *
   * The value must be greater than 0.
   */
  size_t num_fences;

  /**
   * Graphics API-specific options.
   *
   * When the underlying graphics API is
   * - DirectX11, this must be a valid pointer to a FenceSetDeviceD3D11
   *   structure.
   */
  const void* device;
};

/**
 * Create a new fence set.
 *
 * @param out_handle A valid pointer to a handle in which the resulting fence
 * set object is returned.
 * @param fence_set_create_info A valid pointer to a FenceSetCreateInfo
 * structure containing information about how the fence set is to be created.
 * @returns A value indicating success or failure.
 *
 * @note The fence set created will use the EGL context that is current when
 * this function was called.
 */
ELECTRON_EXTERN bool createFenceSet(
    FenceSetHandle* out_handle,
    const FenceSetCreateInfo* fence_set_create_info);

/**
 * Insert a dependency between the main and the other context.
 *
 * This function will create a new fence object, queue a Signal operation into
 * the main context's command buffer, and a Wait operation into the secondary
 * context's command buffer.
 *
 * This function will *not* wait for any previously inserted fences to be
 * signaled and the caller must make sure that there are at most
 * `FenceSetCreateInfo::num_fences - 1` fences in-flight at the same time before
 * calling this function.
 *
 * @param handle The fence set to allocate the fence from.
 * @returns A value indicating success or failure.
 *
 * @note handle must be a valid FenceSetHandle
 *
 * @note When using a fence set in conjunction with an OpenXR swapchain to
 * synchronize work between the main and the presentation context, calling this
 * function after a successful call to `xrWaitSwapchainImage` is always correct,
 * as by that point there must be a fence that has finished executing.
 */
ELECTRON_EXTERN bool insertDependency(FenceSetHandle handle);

/**
 * Destroy a fence set.
 *
 * All GPU work that refers to any of the fences in this set must have
 * completed execution before calling this function.
 *
 * @param handle A valid fence set
 * @returns A valid indicating success or failure.
 */
ELECTRON_EXTERN bool destroyFenceSet(FenceSetHandle handle);

}  // namespace gpu

namespace offscreen {

class ELECTRON_EXTERN PaintObserver {
 public:
  virtual void OnPaint(int dirty_x,
                       int dirty_y,
                       int dirty_width,
                       int dirty_height,
                       int bitmap_width,
                       int bitmap_height,
                       void* data) = 0;

  virtual void OnTexturePaint(const electron::api::gpu::Mailbox& mailbox,
                              const electron::api::gpu::SyncToken& sync_token,
                              int x,
                              int y,
                              int width,
                              int height,
                              bool is_popup,
                              void (*callback)(void*, void*),
                              void* context) = 0;
};

class ELECTRON_EXTERN Canvas {
 public:
  class Observer {
   public:
    virtual void OnCanvasTexturePaint(const electron::api::gpu::Mailbox& mailbox,
                                      const electron::api::gpu::SyncToken& sync_token,
                                      int x,
                                      int y,
                                      int width,
                                      int height,
                                      void (*callback)(void*, void*),
                                      void* context) {}
  };

  class Producer {
   protected:
    void OnPaint(const char* uuid,
                 const ::gpu::Mailbox& mailbox, 
                 const ::gpu::SyncToken& sync_token,
                 int x,
                 int y,
                 int width,
                 int height,
                 void (*callback)(void*, void*),
                 void* context);
  };

  static std::map<std::string, Canvas::Observer*> observers;
};

ELECTRON_EXTERN void __cdecl addOffscreenCanvasPaintObserver(
    const char* uuid, Canvas::Observer* observer);
ELECTRON_EXTERN void __cdecl removeOffscreenCanvasPaintObserver(
    const char* uuid, Canvas::Observer* observer);
ELECTRON_EXTERN void __cdecl addPaintObserver(int id, PaintObserver* observer);
ELECTRON_EXTERN void __cdecl removePaintObserver(int id,
                                                 PaintObserver* observer);
ELECTRON_EXTERN electron::api::gpu::Mailbox __cdecl
createMailboxFromD3D11SharedHandle(void* handle, int width, int height);
ELECTRON_EXTERN void __cdecl
releaseMailbox(electron::api::gpu::Mailbox mailbox);

}  // namespace offscreen
}  // namespace api
}  // namespace electron

#endif  // NATIVE_API_OFFSCREEN
