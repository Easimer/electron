// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NATIVE_API_EGL_CONTEXT_H_
#define NATIVE_API_EGL_CONTEXT_H_

#include <memory>

#include <EGL/egl.h>

#include "base/synchronization/lock.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/swap_buffers_complete_params.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/gpu_memory_buffer.h"
#include "ui/gfx/presentation_feedback.h"

#if BUILDFLAG(IS_MAC)
#include "electron/native_api/egl/overlay_surface.h"
#endif

namespace gpu {
class ServiceDiscardableManager;
class TransferBuffer;

namespace gles2 {
class GLES2CmdHelper;
class GLES2Interface;
}  // namespace gles2
}  // namespace gpu

namespace egl {
class Display;
class Surface;
class Config;

class Context : public base::RefCountedThreadSafe<Context> {
 public:
  Context(Display* display, const Config* config);

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  bool is_current_in_some_thread() const { return is_current_in_some_thread_; }
  void set_is_current_in_some_thread(bool flag) {
    is_current_in_some_thread_ = flag;
  }
  void MarkDestroyed();
  bool SwapBuffers(Surface* current_surface);

  static bool MakeCurrent(Context* current_context,
                          Surface* current_surface,
                          Context* new_context,
                          Surface* new_surface);

  static bool ValidateAttributeList(const EGLint* attrib_list);

  // Called by ThreadState to set the needed global variables when this context
  // is current.
  void ApplyCurrentContext(Surface* surface);
  static void ApplyContextReleased();

  gpu::Mailbox CreateSharedImage(
      gfx::GpuMemoryBuffer* gpu_memory_buffer,
      const gfx::ColorSpace& color_space,
      uint32_t usage);

  void DeleteSharedImage(gpu::Mailbox mailbox);

  bool CreateGpuFences(uint32_t num_fences,
                       uint32_t* buf_ids,
                       std::unique_ptr<gfx::GpuFence>* buf_fences);
  bool DestroyGpuFences(uint32_t num_fences,
                        uint32_t* buf_ids,
                        std::unique_ptr<gfx::GpuFence>* buf_fences);

 private:
  friend class base::RefCountedThreadSafe<Context>;
  ~Context();
  bool ConnectToService(Surface* surface);
  bool ConnectedToService() const;

  void SwapBuffersComplete(Surface* surface,
                           const gpu::SwapBuffersCompleteParams& params,
                           gfx::GpuFenceHandle fence_handle);
  void PresentationComplete(const gfx::PresentationFeedback& feedback);

  bool WasServiceContextLost() const;
  bool IsCompatibleSurface(Surface* surface) const;
  bool Flush();

  static gpu::GpuFeatureInfo platform_gpu_feature_info_;

  Display* display_;
  const Config* config_;
  bool is_current_in_some_thread_;
  bool is_destroyed_;
  bool should_set_draw_rectangle_;

  // base::Lock* lock_ = nullptr;

#if BUILDFLAG(IS_MAC)
  OverlaySurface* overlay_surface_;
#endif

  scoped_refptr<viz::ContextProviderCommandBuffer> context_provider_;
};

}  // namespace egl

#endif  // NATIVE_API_EGL_CONTEXT_H_
