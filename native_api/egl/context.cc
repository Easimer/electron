// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "electron/native_api/egl/context.h"

#include <GLES2/gl2extchromium.h>

#include "base/run_loop.h"
#include "base/trace_event/trace_log.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/gpu_utils.h"
#include "content/public/common/gpu_stream_constants.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/client/gles2_lib.h"
#include "gpu/command_buffer/common/context_creation_attribs.h"
#include "gpu/ipc/client/gpu_channel_host.h"

#include "electron/native_api/egl/config.h"
#include "electron/native_api/egl/display.h"
#include "electron/native_api/egl/surface.h"
#include "electron/native_api/egl/thread_state.h"
#include "ui/gfx/color_space.h"
#include "ui/gl/init/gl_factory.h"

#if BUILDFLAG(IS_WIN)
#include "ui/gfx/win/rendering_window_manager.h"
#endif

#if BUILDFLAG(IS_MAC)
#include "ui/accelerated_widget_mac/ca_layer_frame_sink.h"
#endif

#include "base/logging.h"

namespace {
const bool kBindGeneratesResources = true;
const bool kLoseContextWhenOutOfMemory = false;
}  // namespace

namespace egl {

Context::Context(Display* display, const Config* config)
    : display_(display),
      config_(config),
      is_current_in_some_thread_(false),
      is_destroyed_(false),
      should_set_draw_rectangle_(true) {}

Context::~Context() {}

void Context::MarkDestroyed() {
  is_destroyed_ = true;
}

bool Context::SwapBuffers(Surface* current_surface) {
  DCHECK(is_current_in_some_thread_);
  if (WasServiceContextLost())
    return false;
  const gfx::Size& size = current_surface->size();
  bool offscreen = current_surface->is_offscreen();

  if (!offscreen && current_surface->is_size_dirty() && !size.IsEmpty()) {
#if BUILDFLAG(IS_MAC)
    overlay_surface_->Reshape(size, current_surface->scale_factor());
#else
    gfx::ColorSpace color_space = gfx::ColorSpace::CreateSRGB();
    context_provider_->ContextGL()->ResizeCHROMIUM(
        size.width(), size.height(), current_surface->scale_factor(),
        color_space.AsGLColorSpace(), true);
#endif
    current_surface->set_size_dirty(false);
  }

#if BUILDFLAG(IS_MAC)
  overlay_surface_->SwapBuffers();
#endif

  auto swap_callback =
      base::BindOnce(&Context::SwapBuffersComplete, base::Unretained(this),
                     base::Unretained(current_surface));
  auto presentation_callback =
      base::BindOnce(&Context::PresentationComplete, base::Unretained(this));
  context_provider_->ContextSupport()->Swap(0, std::move(swap_callback),
                                            std::move(presentation_callback));

  if (should_set_draw_rectangle_ && !offscreen && !size.IsEmpty()) {
    context_provider_->ContextGL()->SetDrawRectangleCHROMIUM(0, 0, size.width(),
                                                             size.height());

    if (context_provider_->ContextGL()->GetError() == GL_INVALID_OPERATION) {
      should_set_draw_rectangle_ = false;
    }
  }

#if BUILDFLAG(IS_MAC)
  overlay_surface_->SwapBuffersComplete();
#endif

  context_provider_->ContextGL()->ShallowFlushCHROMIUM();

  return true;
}

void Context::SwapBuffersComplete(
    Surface* surface,
    const gpu::SwapBuffersCompleteParams& params,
    gfx::GpuFenceHandle fence_handle) {
  // send the params
#if BUILDFLAG(IS_MAC)
  if (!params.ca_layer_params.is_empty && !surface->is_offscreen()) {
    ui::CALayerFrameSink* ca_layer_frame_sink =
        ui::CALayerFrameSink::FromAcceleratedWidget(surface->window());
    if (ca_layer_frame_sink)
      ca_layer_frame_sink->UpdateCALayerTree(params.ca_layer_params);
  }
#endif
}

static void GpuFenceArrived(std::unique_ptr<gfx::GpuFence>* target,
                            base::OnceClosure cb,
                            std::unique_ptr<gfx::GpuFence> fence) {
  *target = std::move(fence);

  if (cb) {
    std::move(cb).Run();
  }
}

bool Context::CreateGpuFences(uint32_t num_fences,
                              uint32_t* buf_ids,
                              std::unique_ptr<gfx::GpuFence>* buf_fences) {
  auto* gl = context_provider_->ContextGL();
  auto* support = context_provider_->ContextSupport();

  if (!gl || !support) {
    return false;
  }

  base::RunLoop runLoop;
  for (uint32_t idx_fence = 0; idx_fence < num_fences; idx_fence++) {
    GLuint id = gl->CreateGpuFenceCHROMIUM();

    base::OnceClosure cb;
    if (idx_fence == num_fences - 1) {
      cb = runLoop.QuitClosure();
    }

    support->GetGpuFence(
        id, base::BindOnce(&GpuFenceArrived, &buf_fences[idx_fence],
                           std::move(cb)));
    buf_ids[idx_fence] = id;
  }

  runLoop.Run();

  bool free_fences = false;

  for (uint32_t idx_fence = 0; idx_fence < num_fences; idx_fence++) {
    if (buf_fences[idx_fence] == nullptr) {
      free_fences |= true;
      break;
    }
  }

  if (free_fences) {
    for (uint32_t idx_fence = 0; idx_fence < num_fences; idx_fence++) {
      buf_fences[idx_fence].reset();
      gl->DestroyGpuFenceCHROMIUM(buf_ids[idx_fence]);
    }

    return false;
  }

  return true;
}

bool Context::DestroyGpuFences(uint32_t num_fences,
                               uint32_t* buf_ids,
                               std::unique_ptr<gfx::GpuFence>* buf_fences) {
  if (num_fences == 0) {
    return true;
  }

  if (buf_ids == nullptr) {
    return false;
  }

  auto* gl = context_provider_->ContextGL();
  if (gl == nullptr) {
    return false;
  }

  for (uint32_t idx_fence = 0; idx_fence < num_fences; idx_fence++) {
    if (buf_fences[idx_fence] != nullptr) {
      buf_fences[idx_fence].reset();
      gl->DestroyGpuFenceCHROMIUM(buf_ids[idx_fence]);
    }
    buf_ids[idx_fence] = 0;
  }

  return true;
}

void Context::PresentationComplete(const gfx::PresentationFeedback& feedback) {
  // NOOP
}

bool Context::MakeCurrent(Context* current_context,
                          Surface* current_surface,
                          Context* new_context,
                          Surface* new_surface) {
  if (!new_context && !current_context) {
    return true;
  }

  if (new_context) {
    if (!new_context->IsCompatibleSurface(new_surface))
      return false;

    if (new_context->ConnectedToService()) {
      if (new_context->WasServiceContextLost())
        return false;
      if (new_context != current_context || new_surface != current_surface) {
        Context::ApplyContextReleased();
        // if (current_context->lock_ != nullptr)
        //   current_context->lock_->Release();
        new_context->ApplyCurrentContext(new_surface);
      }
    } else {
      if (!new_context->ConnectToService(new_surface)) {
        return false;
      } else {
        Context::ApplyContextReleased();
        // if (current_context->lock_ != nullptr)
        //   current_context->lock_->Release();
        new_context->ApplyCurrentContext(new_surface);
      }
    }
  }

  return true;
}

bool Context::ValidateAttributeList(const EGLint* attrib_list) {
  if (attrib_list) {
    for (int i = 0; attrib_list[i] != EGL_NONE; attrib_list += 2) {
      switch (attrib_list[i]) {
        case EGL_CONTEXT_CLIENT_VERSION:
          break;
        default:
          return false;
      }
    }
  }
  return true;
}

void Context::ApplyCurrentContext(Surface* surface) {
  gles2::SetGLContext(context_provider_->ContextGL());
}

void Context::ApplyContextReleased() {
  gles2::SetGLContext(nullptr);
}

gpu::Mailbox Context::CreateSharedImage(
    gfx::GpuMemoryBuffer* gpu_memory_buffer,
    const gfx::ColorSpace& color_space,
    uint32_t usage) {
  auto* sii = context_provider_->SharedImageInterface();

  auto mailbox = sii->CreateSharedImage(
      gpu_memory_buffer,
      nullptr,
      gfx::BufferPlane::DEFAULT,
      color_space,
      GrSurfaceOrigin::kTopLeft_GrSurfaceOrigin,
      SkAlphaType::kUnpremul_SkAlphaType,
      usage);

  auto creation_token = sii->GenUnverifiedSyncToken();
  context_provider_->ContextGL()->WaitSyncTokenCHROMIUM(
      creation_token.GetConstData());

  return mailbox;
}

void Context::DeleteSharedImage(gpu::Mailbox mailbox) {
  auto* sii = context_provider_->SharedImageInterface();

  auto deletion_token = sii->GenUnverifiedSyncToken();
  sii->DestroySharedImage(deletion_token, mailbox);
}

bool Context::ConnectToService(Surface* surface) {
  gpu::GpuChannelEstablishFactory* factory =
      content::GetGpuChannelEstablishFactory();
  scoped_refptr<gpu::GpuChannelHost> host = factory->EstablishGpuChannelSync();

  gpu::ContextCreationAttribs helper;
  config_->GetAttrib(EGL_ALPHA_SIZE, &helper.alpha_size);
  config_->GetAttrib(EGL_DEPTH_SIZE, &helper.depth_size);
  config_->GetAttrib(EGL_STENCIL_SIZE, &helper.stencil_size);

  helper.gpu_preference = gl::GpuPreference::kHighPerformance;
  helper.buffer_preserved = false;
  helper.bind_generates_resource = kBindGeneratesResources;
  helper.fail_if_major_perf_caveat = false;
  helper.lose_context_when_out_of_memory = kLoseContextWhenOutOfMemory;
  helper.should_use_native_gmb_for_backbuffer = true;
  helper.context_type = gpu::CONTEXT_TYPE_OPENGLES3;
  helper.color_space = gpu::COLOR_SPACE_SRGB;

  gpu::SurfaceHandle surface_handle;

  if (surface->is_offscreen()) {
    surface_handle = gpu::kNullSurfaceHandle;
    helper.offscreen_framebuffer_size = surface->size();
  } else {
    surface_handle = surface->window();
  }

#if BUILDFLAG(IS_WIN)
  gfx::RenderingWindowManager::GetInstance()->RegisterParent(surface->window());
#endif

  context_provider_ = base::MakeRefCounted<viz::ContextProviderCommandBuffer>(
      host, factory->GetGpuMemoryBufferManager(), content::kGpuStreamIdDefault,
      gpu::SchedulingPriority::kNormal, surface_handle,
      GURL(std::string("electron://gpu/command_buffer")),
      true /* automatic flushes */, false /* support locking */,
      false /* support grcontext */, gpu::SharedMemoryLimits(), helper,
      viz::command_buffer_metrics::ContextType::RENDER_COMPOSITOR);

  gpu::ContextResult bind_result = context_provider_->BindToCurrentThread();
  // TODO: we could handle transient failures and retry
  if (bind_result == gpu::ContextResult::kSuccess) {
    base::trace_event::TraceLog::GetInstance()->SetCurrentThreadBlocksMessageLoop();

    const auto& context_capabilities = context_provider_->ContextCapabilities();
    should_set_draw_rectangle_ = context_capabilities.dc_layers;

    context_provider_->ContextSupport()->SetAggressivelyFreeResources(true);

#if BUILDFLAG(IS_MAC)
    overlay_surface_ = new OverlaySurface(context_provider_, surface_handle);
#endif

    return true;
  } else {
    context_provider_.reset();
    return false;
  }
}

bool Context::ConnectedToService() const {
  return context_provider_.get() != nullptr;
}

bool Context::WasServiceContextLost() const {
  return false;
}

bool Context::IsCompatibleSurface(Surface* surface) const {
  EGLint value = EGL_NONE;
  config_->GetAttrib(EGL_SURFACE_TYPE, &value);
  bool context_config_is_offscreen = (value & EGL_PBUFFER_BIT) != 0;
  surface->config()->GetAttrib(EGL_SURFACE_TYPE, &value);
  bool surface_config_is_offscreen = (value & EGL_PBUFFER_BIT) != 0;
  return surface_config_is_offscreen == context_config_is_offscreen;
}

bool Context::Flush() {
  if (WasServiceContextLost())
    return false;
  context_provider_->ContextGL()->Flush();
  return true;
}

}  // namespace egl
