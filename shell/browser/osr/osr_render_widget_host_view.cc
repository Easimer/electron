// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/osr/osr_render_widget_host_view.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/callback_helpers.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/task/post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/time/time.h"
#include "components/viz/common/features.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/frame_sinks/delay_based_time_source.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/surfaces/frame_sink_id_allocator.h"
#include "content/browser/gpu/gpu_data_manager_impl.h"  // nogncheck
#include "content/browser/renderer_host/cursor_manager.h"  // nogncheck
#include "content/browser/renderer_host/input/synthetic_gesture_target.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_delegate.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_input_event_router.h" // nogncheck
#include "content/browser/renderer_host/render_widget_host_owner_delegate.h"  // nogncheck
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/context_factory.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/render_process_host.h"
#include "gpu/command_buffer/client/gl_helper.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "media/base/video_frame.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_type.h"
#include "ui/display/screen.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/dip_util.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/gfx/skbitmap_operations.h"
#include "ui/latency/latency_info.h"

namespace electron {

namespace {

const float kDefaultScaleFactor = 1.0;

const float kAutoScaleFactor = 0.0f;

base::TimeDelta TimeDeltaFromHz(double frequency) {
  return base::Seconds(1) / frequency;
}

ui::MouseEvent UiMouseEventFromWebMouseEvent(blink::WebMouseEvent event) {
  ui::EventType type = ui::EventType::ET_UNKNOWN;
  switch (event.GetType()) {
    case blink::WebInputEvent::Type::kMouseDown:
      type = ui::EventType::ET_MOUSE_PRESSED;
      break;
    case blink::WebInputEvent::Type::kMouseUp:
      type = ui::EventType::ET_MOUSE_RELEASED;
      break;
    case blink::WebInputEvent::Type::kMouseMove:
      type = ui::EventType::ET_MOUSE_MOVED;
      break;
    case blink::WebInputEvent::Type::kMouseEnter:
      type = ui::EventType::ET_MOUSE_ENTERED;
      break;
    case blink::WebInputEvent::Type::kMouseLeave:
      type = ui::EventType::ET_MOUSE_EXITED;
      break;
    case blink::WebInputEvent::Type::kMouseWheel:
      type = ui::EventType::ET_MOUSEWHEEL;
      break;
    default:
      type = ui::EventType::ET_UNKNOWN;
      break;
  }

  int button_flags = 0;
  switch (event.button) {
    case blink::WebMouseEvent::Button::kBack:
      button_flags |= ui::EventFlags::EF_BACK_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kForward:
      button_flags |= ui::EventFlags::EF_FORWARD_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kLeft:
      button_flags |= ui::EventFlags::EF_LEFT_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kMiddle:
      button_flags |= ui::EventFlags::EF_MIDDLE_MOUSE_BUTTON;
      break;
    case blink::WebMouseEvent::Button::kRight:
      button_flags |= ui::EventFlags::EF_RIGHT_MOUSE_BUTTON;
      break;
    default:
      button_flags = 0;
      break;
  }

  ui::MouseEvent ui_event(type,
                          gfx::Point(std::floor(event.PositionInWidget().x()),
                                     std::floor(event.PositionInWidget().y())),
                          gfx::Point(std::floor(event.PositionInWidget().x()),
                                     std::floor(event.PositionInWidget().y())),
                          ui::EventTimeForNow(), button_flags, button_flags);
  if (event.click_count > 0)
    ui_event.SetClickCount(event.click_count);

  return ui_event;
}

ui::MouseWheelEvent UiMouseWheelEventFromWebMouseEvent(
    blink::WebMouseWheelEvent event) {
  return ui::MouseWheelEvent(UiMouseEventFromWebMouseEvent(event),
                             std::floor(event.delta_x),
                             std::floor(event.delta_y));
}

}  // namespace

class ElectronDelegatedFrameHostClient
    : public content::DelegatedFrameHostClient {
 public:
  explicit ElectronDelegatedFrameHostClient(OffScreenRenderWidgetHostView* view)
      : view_(view) {}

  // disable copy
  ElectronDelegatedFrameHostClient(const ElectronDelegatedFrameHostClient&) =
      delete;
  ElectronDelegatedFrameHostClient& operator=(
      const ElectronDelegatedFrameHostClient&) = delete;

  ui::Layer* DelegatedFrameHostGetLayer() const override {
    return view_->GetRootLayer();
  }

  bool DelegatedFrameHostIsVisible() const override {
    return view_->IsShowing();
  }

  SkColor DelegatedFrameHostGetGutterColor() const override {
    return SK_ColorWHITE;
  }

  void OnFrameTokenChanged(uint32_t frame_token,
                           base::TimeTicks activation_time) override {
    view_->render_widget_host()->DidProcessFrame(frame_token, activation_time);
  }

  float GetDeviceScaleFactor() const override {
    return view_->GetDeviceScaleFactor();
  }

  std::vector<viz::SurfaceId> CollectSurfaceIdsForEviction() override {
    return view_->render_widget_host()->CollectSurfaceIdsForEviction();
  }

  void InvalidateLocalSurfaceIdOnEviction() override {
    view_->InvalidateLocalSurfaceId();
  }

  bool ShouldShowStaleContentOnEviction() override { return false; }

 private:
  OffScreenRenderWidgetHostView* const view_;
};

class StandaloneInitializer
    : public OffScreenRenderWidgetHostView::Initializer {
 public:
  StandaloneInitializer(bool transparent,
                        OnPaintCallback paint_callback,
                        OnTexturePaintCallback texture_paint_callback,
                        gfx::Size initial_size)
      : transparent_(transparent),
        paint_callback_(paint_callback),
        texture_paint_callback_(texture_paint_callback),
        initial_size_(initial_size) {}

  bool IsTransparent() const override { return transparent_; }

  const OnPaintCallback& GetPaintCallback() const override {
    return paint_callback_;
  }

  const OnTexturePaintCallback& GetTexturePaintCallback() const override {
    return texture_paint_callback_;
  }

  gfx::Size GetInitialSize() const override { return initial_size_; }

 private:
  bool transparent_;
  OnPaintCallback paint_callback_;
  OnTexturePaintCallback texture_paint_callback_;
  gfx::Size initial_size_;
};

OffScreenRenderWidgetHostView::OffScreenRenderWidgetHostView(
    Initializer* initializer,
    content::RenderWidgetHost* host,
    OffScreenRenderWidgetHostView* parent,
    bool painting,
    int frame_rate,
    float scale_factor)
    : content::RenderWidgetHostViewBase(host),
      frame_rate_(frame_rate),
      cursor_manager_(std::make_unique<content::CursorManager>(this)),
      render_widget_host_(content::RenderWidgetHostImpl::From(host)),
      parent_host_view_(parent),
      backing_(new SkBitmap),
      painting_(painting),
      mouse_wheel_phase_handler_(this),
      weak_ptr_factory_(this) {
  DCHECK(render_widget_host_);
  DCHECK(!render_widget_host_->GetView());

  transparent_ = initializer->IsTransparent();
  callback_ = initializer->GetPaintCallback();
  texture_callback_ = initializer->GetTexturePaintCallback();
  size_ = initializer->GetInitialSize();

  manual_device_scale_factor_ = scale_factor;
  current_device_scale_factor_ = kDefaultScaleFactor;

  // Initialize a screen_infos_ struct as needed, to cache the scale factor.
  if (screen_infos_.screen_infos.empty()) {
    UpdateScreenInfo();
  }
  screen_infos_.mutable_current().device_scale_factor = kDefaultScaleFactor;

  delegated_frame_host_client_ =
      std::make_unique<ElectronDelegatedFrameHostClient>(this);
  delegated_frame_host_ = std::make_unique<content::DelegatedFrameHost>(
      AllocateFrameSinkId(), delegated_frame_host_client_.get(),
      false /* should_register_frame_sink_id */);

  root_layer_ = std::make_unique<ui::Layer>(ui::LAYER_SOLID_COLOR);

  bool opaque = SkColorGetA(background_color_) == SK_AlphaOPAQUE;
  GetRootLayer()->SetFillsBoundsOpaquely(opaque);
  GetRootLayer()->SetColor(background_color_);

  ui::ContextFactory* context_factory = content::GetContextFactory();
  compositor_ = std::make_unique<ui::Compositor>(
      context_factory->AllocateFrameSinkId(),
      context_factory,
      base::ThreadTaskRunnerHandle::Get(),
      false /* enable_pixel_canvas */,
      false /* use_external_begin_frame_control */,
      false /* force_software_compositor */,
      false /* enable_compositing_based_throttling */,
      true /* enable_manual_begin_frame_control */);
  compositor_->SetAcceleratedWidget(gfx::kNullAcceleratedWidget);
  compositor_->SetDelegate(this);
  compositor_->SetRootLayer(root_layer_.get());
  compositor_->AddChildFrameSink(GetFrameSinkId());

#if BUILDFLAG(IS_WIN)
  auto* const gpu_data_manager = content::GpuDataManagerImpl::GetInstance();
  compositor_->SetShouldDisableSwapUntilResize(
      gpu_data_manager->GetGPUInfo().overlay_info.direct_composition);
#endif

  // This may result in a call to GetFrameSinkId().
  render_widget_host_->SetView(this);

  if (render_widget_host_->delegate() &&
      render_widget_host_->delegate()->GetInputEventRouter()) {
    render_widget_host_->delegate()->GetInputEventRouter()->AddFrameSinkIdOwner(
        GetFrameSinkId(), this);
  }

  if (!parent_host_view_) {
    SetRootLayerSize(false);
    if (!render_widget_host_->is_hidden()) {
      Show();
    }

    SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                                absl::nullopt);
  }

  UpdateScreenInfo();
}

OffScreenRenderWidgetHostView::~OffScreenRenderWidgetHostView() {
  // Marking the DelegatedFrameHost as removed from the window hierarchy is
  // necessary to remove all connections to its old ui::Compositor.
  if (is_showing_)
    GetDelegatedFrameHost()->WasHidden(
        content::DelegatedFrameHost::HiddenCause::kOther);
  GetDelegatedFrameHost()->DetachFromCompositor();

  delegated_frame_host_.reset();
  compositor_.reset();
  root_layer_.reset();
}

void OffScreenRenderWidgetHostView::InitAsChild(gfx::NativeView) {
  DCHECK(parent_host_view_);

  if (parent_host_view_->child_host_view_) {
    parent_host_view_->child_host_view_->CancelWidget();
  }

  parent_host_view_->set_child_host_view(this);
  parent_host_view_->Hide();

  SetRootLayerSize(false);
  Show();
  SetPainting(parent_host_view_->IsPainting());
}

void OffScreenRenderWidgetHostView::SetSize(const gfx::Size& size) {
  if (IsPopupWidget()) {
    popup_position_.set_size(size);

    SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                                absl::nullopt);
  } else {
    size_ = size;
    WasResized();
  }
}

void OffScreenRenderWidgetHostView::SetBounds(const gfx::Rect& new_bounds) {
  if (IsPopupWidget()) {
    popup_position_ = new_bounds;

    SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                                absl::nullopt);
  } else {
    SetSize(new_bounds.size());
  }
}

gfx::NativeView OffScreenRenderWidgetHostView::GetNativeView() {
  return gfx::NativeView();
}

gfx::NativeViewAccessible
OffScreenRenderWidgetHostView::GetNativeViewAccessible() {
  return gfx::NativeViewAccessible();
}

void OffScreenRenderWidgetHostView::Focus() {}

bool OffScreenRenderWidgetHostView::HasFocus() {
  return false;
}

bool OffScreenRenderWidgetHostView::IsSurfaceAvailableForCopy() {
  if (GetDelegatedFrameHost()) {
    return GetDelegatedFrameHost()->CanCopyFromCompositingSurface();
  }

  return false;
}

void OffScreenRenderWidgetHostView::ShowWithVisibility(
    content::PageVisibilityState /*page_visibility*/) {
  if (is_showing_)
    return;

  is_showing_ = true;

  if (!GetLocalSurfaceId().is_valid()) {
    AllocateLocalSurfaceId();
    SynchronizeVisualProperties(cc::DeadlinePolicy::UseDefaultDeadline(),
                                GetLocalSurfaceId());
  }

  if (render_widget_host_) {
    render_widget_host_->WasShown({});
  }

  if (GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->AttachToCompositor(compositor_.get());
    GetDelegatedFrameHost()->WasShown(
        GetLocalSurfaceId(), GetRootLayer()->bounds().size(), {});
  }
}

void OffScreenRenderWidgetHostView::Hide() {
  if (!is_showing_)
    return;

  if (render_widget_host_)
    render_widget_host_->WasHidden();

  if (GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->WasHidden(
        content::DelegatedFrameHost::HiddenCause::kOther);
    GetDelegatedFrameHost()->DetachFromCompositor();
  }

  is_showing_ = false;
}

bool OffScreenRenderWidgetHostView::IsShowing() {
  return is_showing_;
}

void OffScreenRenderWidgetHostView::EnsureSurfaceSynchronizedForWebTest() {
  ++latest_capture_sequence_number_;
  SynchronizeVisualProperties(cc::DeadlinePolicy::UseInfiniteDeadline(),
                              absl::nullopt);
}

gfx::Rect OffScreenRenderWidgetHostView::GetViewBounds() {
  if (IsPopupWidget()) {
    return popup_position_;
  }

  return gfx::Rect(SizeInPixels());
}

void OffScreenRenderWidgetHostView::SetBackgroundColor(SkColor color) {
  // The renderer will feed its color back to us with the first CompositorFrame.
  // We short-cut here to show a sensible color before that happens.
  UpdateBackgroundColorFromRenderer(color);

  content::RenderWidgetHostViewBase::SetBackgroundColor(color);
}

absl::optional<SkColor> OffScreenRenderWidgetHostView::GetBackgroundColor() {
  return background_color_;
}

void OffScreenRenderWidgetHostView::UpdateBackgroundColor() {}

blink::mojom::PointerLockResult OffScreenRenderWidgetHostView::LockMouse(
    bool request_unadjusted_movement) {
  return blink::mojom::PointerLockResult::kPermissionDenied;
}

blink::mojom::PointerLockResult OffScreenRenderWidgetHostView::ChangeMouseLock(
    bool request_unadjusted_movement) {
  return blink::mojom::PointerLockResult::kPermissionDenied;
}

void OffScreenRenderWidgetHostView::UnlockMouse() {}

void OffScreenRenderWidgetHostView::TakeFallbackContentFrom(
    content::RenderWidgetHostView* view) {
  DCHECK(!static_cast<content::RenderWidgetHostViewBase*>(view)
              ->IsRenderWidgetHostViewChildFrame());
  auto* view_osr = static_cast<OffScreenRenderWidgetHostView*>(view);
  SetBackgroundColor(view_osr->background_color_);
  if (GetDelegatedFrameHost() && view_osr->GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->TakeFallbackContentFrom(
        view_osr->GetDelegatedFrameHost());
  }
  host()->GetContentRenderingTimeoutFrom(view_osr->host());
}

#if defined(OS_MAC)
void OffScreenRenderWidgetHostView::SetActive(bool active) {}

void OffScreenRenderWidgetHostView::ShowDefinitionForSelection() {}

void OffScreenRenderWidgetHostView::SpeakSelection() {}

void OffScreenRenderWidgetHostView::SetWindowFrameInScreen(
    const gfx::Rect& rect) {}
#endif

void OffScreenRenderWidgetHostView::ResetFallbackToFirstNavigationSurface() {
  if (GetDelegatedFrameHost()) {
    GetDelegatedFrameHost()->ResetFallbackToFirstNavigationSurface();
  }
}

void OffScreenRenderWidgetHostView::InitAsPopup(
    content::RenderWidgetHostView* parent_host_view,
    const gfx::Rect& pos,
    const gfx::Rect& anchor_rect) {
  DCHECK_EQ(parent_host_view_, parent_host_view);
  DCHECK_EQ(widget_type_, content::WidgetType::kPopup);

  if (parent_host_view_->popup_host_view_) {
    parent_host_view_->popup_host_view_->CancelWidget();
  }

  parent_host_view_->set_popup_host_view(this);
  parent_callback_ =
      base::BindRepeating(&OffScreenRenderWidgetHostView::OnPopupPaint,
                          parent_host_view_->weak_ptr_factory_.GetWeakPtr());
  parent_texture_callback_ =
        base::BindRepeating(&OffScreenRenderWidgetHostView::OnPopupTexturePaint,
                            parent_host_view_->weak_ptr_factory_.GetWeakPtr());

  SetBounds(pos);

  Show();
  SetPainting(true);
  Invalidate();
}

void OffScreenRenderWidgetHostView::UpdateCursor(const content::WebCursor&) {}

void OffScreenRenderWidgetHostView::SetIsLoading(bool loading) {}

void OffScreenRenderWidgetHostView::RenderProcessGone() {
  Destroy();
}

void OffScreenRenderWidgetHostView::Destroy() {
  if (!is_destroyed_) {
    is_destroyed_ = true;

    if (parent_host_view_ != nullptr) {
      CancelWidget();
    } else {
      if (popup_host_view_)
        popup_host_view_->CancelWidget();
      if (child_host_view_)
        child_host_view_->CancelWidget();
      for (auto* proxy_view : proxy_views_)
        proxy_view->RemoveObserver();
      Hide();
    }
  }

  delete this;
}

void OffScreenRenderWidgetHostView::UpdateTooltipUnderCursor(
    const std::u16string&) {}

content::CursorManager* OffScreenRenderWidgetHostView::GetCursorManager() {
  return cursor_manager_.get();
}

gfx::Size OffScreenRenderWidgetHostView::GetCompositorViewportPixelSize() {
  return gfx::ScaleToCeiledSize(GetRequestedRendererSize(), GetScaleFactor());
}

uint32_t OffScreenRenderWidgetHostView::GetCaptureSequenceNumber() const {
  return latest_capture_sequence_number_;
}

content::RenderWidgetHostViewBase*
OffScreenRenderWidgetHostView::CreateViewForWidget(
    content::RenderWidgetHost* render_widget_host,
    content::RenderWidgetHost* embedder_render_widget_host,
    content::WebContentsView* web_contents_view) {
  if (render_widget_host->GetView()) {
    return static_cast<content::RenderWidgetHostViewBase*>(
        render_widget_host->GetView());
  }

  OffScreenRenderWidgetHostView* embedder_host_view = nullptr;
  if (embedder_render_widget_host) {
    embedder_host_view = static_cast<OffScreenRenderWidgetHostView*>(
        embedder_render_widget_host->GetView());
  }

  StandaloneInitializer initializer(transparent_, callback_, texture_callback_,
                                    size());
  return new OffScreenRenderWidgetHostView(
      &initializer, render_widget_host, embedder_host_view, true,
      embedder_host_view->GetFrameRate(), embedder_host_view->GetScaleFactor());
}

void OffScreenRenderWidgetHostView::CopyFromSurface(
    const gfx::Rect& src_rect,
    const gfx::Size& output_size,
    base::OnceCallback<void(const SkBitmap&)> callback) {
  if (GetDelegatedFrameHost())
    GetDelegatedFrameHost()->CopyFromCompositingSurface(src_rect, output_size,
                                                        std::move(callback));
}

display::ScreenInfos
OffScreenRenderWidgetHostView::GetNewScreenInfosForUpdate() {
  display::ScreenInfos screen_infos;
  screen_infos.screen_infos =
      std::vector<display::ScreenInfo>({ display::ScreenInfo() });

  screen_infos.current_display_id = display::kDefaultDisplayId;

  screen_infos.mutable_current().depth = 24;
  screen_infos.mutable_current().depth_per_component = 8;
  screen_infos.mutable_current().orientation_angle = 0;
  screen_infos.mutable_current().orientation_type =
      display::mojom::ScreenOrientation::kLandscapePrimary;
  screen_infos.mutable_current().rect = gfx::Rect(size_);
  screen_infos.mutable_current().available_rect = gfx::Rect(size_);
  if (!IsPopupWidget()) {
    screen_infos.mutable_current().device_scale_factor = GetScaleFactor();
  }

  return screen_infos;
}

void OffScreenRenderWidgetHostView::TransformPointToRootSurface(
    gfx::PointF* point) {}

gfx::Rect OffScreenRenderWidgetHostView::GetBoundsInRootWindow() {
  if (IsPopupWidget()) {
    return popup_position_;
  }

  return gfx::Rect(size_);
}

#if !defined(OS_MAC)
viz::ScopedSurfaceIdAllocator
OffScreenRenderWidgetHostView::DidUpdateVisualProperties(
    const cc::RenderFrameMetadata& metadata) {
  base::OnceCallback<void()> allocation_task = base::BindOnce(
      &OffScreenRenderWidgetHostView::OnDidUpdateVisualPropertiesComplete,
      weak_ptr_factory_.GetWeakPtr(), metadata);
  return viz::ScopedSurfaceIdAllocator(std::move(allocation_task));
}
#endif

absl::optional<content::DisplayFeature>
OffScreenRenderWidgetHostView::GetDisplayFeature() {
  return absl::nullopt;
}

void OffScreenRenderWidgetHostView::SetDisplayFeatureForTesting(
    const content::DisplayFeature* display_feature) {}

viz::SurfaceId OffScreenRenderWidgetHostView::GetCurrentSurfaceId() const {
  return GetDelegatedFrameHost()
             ? GetDelegatedFrameHost()->GetCurrentSurfaceId()
             : viz::SurfaceId();
}

void OffScreenRenderWidgetHostView::ImeCompositionRangeChanged(
    const gfx::Range&,
    const std::vector<gfx::Rect>&) {}

std::unique_ptr<content::SyntheticGestureTarget>
OffScreenRenderWidgetHostView::CreateSyntheticGestureTarget() {
  NOTIMPLEMENTED();
  return nullptr;
}

ui::Compositor* OffScreenRenderWidgetHostView::GetCompositor() {
  return compositor_.get();
}

bool OffScreenRenderWidgetHostView::TransformPointToCoordSpaceForView(
    const gfx::PointF& point,
    RenderWidgetHostViewBase* target_view,
    gfx::PointF* transformed_point) {
  if (target_view == this) {
    *transformed_point = point;
    return true;
  }

  return false;
}

void OffScreenRenderWidgetHostView::DidNavigate() {
  if (!IsShowing()) {
    // Navigating while hidden should not allocate a new LocalSurfaceID. Once
    // sizes are ready, or we begin to Show, we can then allocate the new
    // LocalSurfaceId.
    InvalidateLocalSurfaceId();
  } else {
    if (is_first_navigation_) {
      InstallTransparency();
      // The first navigation does not need a new LocalSurfaceID. The renderer
      // can use the ID that was already provided.
      SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                                  GetLocalSurfaceId());
    } else {
      SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                                  absl::nullopt);
    }
  }

  if (GetDelegatedFrameHost())
    GetDelegatedFrameHost()->DidNavigate();
  is_first_navigation_ = false;
}

const viz::FrameSinkId& OffScreenRenderWidgetHostView::GetFrameSinkId() const {
  return GetDelegatedFrameHost()
             ? GetDelegatedFrameHost()->frame_sink_id()
             : viz::FrameSinkIdAllocator::InvalidFrameSinkId();
}

viz::FrameSinkId OffScreenRenderWidgetHostView::GetRootFrameSinkId() {
  return GetCompositor() ? GetCompositor()->frame_sink_id()
                         : viz::FrameSinkId();
}

bool OffScreenRenderWidgetHostView::IsOffscreen() const {
  return true;
}

std::unique_ptr<viz::HostDisplayClient>
OffScreenRenderWidgetHostView::CreateHostDisplayClient(
    ui::Compositor* compositor) {
  host_display_client_ = new OffScreenHostDisplayClient(
      gfx::kNullAcceleratedWidget,
      base::BindRepeating(&OffScreenRenderWidgetHostView::OnPaint,
                          weak_ptr_factory_.GetWeakPtr()),
      base::BindRepeating(&OffScreenRenderWidgetHostView::OnTexturePaint,
                          weak_ptr_factory_.GetWeakPtr()),
      base::BindRepeating(&OffScreenRenderWidgetHostView::OnBackingTextureCreated,
                          weak_ptr_factory_.GetWeakPtr()));
  host_display_client_->SetActive(IsPainting());
  return base::WrapUnique(host_display_client_);
}

bool OffScreenRenderWidgetHostView::InstallTransparency() {
  if (transparent_) {
    SetBackgroundColor(SK_ColorTRANSPARENT);
    if (GetCompositor()) {
      GetCompositor()->SetBackgroundColor(SK_ColorTRANSPARENT);
    }
    return true;
  }
  return false;
}

#if BUILDFLAG(IS_MAC)
void OffScreenRenderWidgetHostView::ShowSharePicker(
    const std::string& title,
    const std::string& text,
    const std::string& url,
    const std::vector<std::string>& file_paths,
    blink::mojom::ShareService::ShareCallback callback) {}
#endif

void OffScreenRenderWidgetHostView::WasResized() {
  // Only one resize will be in-flight at a time.
  if (hold_resize_) {
    if (!pending_resize_)
      pending_resize_ = true;
    return;
  }

  SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                              absl::nullopt);
}

void OffScreenRenderWidgetHostView::SynchronizeVisualProperties(
    const cc::DeadlinePolicy& deadline_policy,
    const absl::optional<viz::LocalSurfaceId>& child_local_surface_id) {
  SetupFrameRate();

  const bool resized = ResizeRootLayer();
  bool surface_id_updated = false;

  if (!resized && child_local_surface_id) {
    // Update the current surface ID.
    parent_local_surface_id_allocator_->UpdateFromChild(
        *child_local_surface_id);
    surface_id_updated = true;
  }

  // Allocate a new surface ID if the surface has been resized or if the current
  // ID is invalid (meaning we may have been evicted).
  if (resized || !GetOrCreateLocalSurfaceId().is_valid()) {
    AllocateLocalSurfaceId();
    surface_id_updated = true;
  }

  if (surface_id_updated) {
    GetDelegatedFrameHost()->EmbedSurface(
        GetOrCreateLocalSurfaceId(), GetViewBounds().size(), deadline_policy);

    // |render_widget_host_| will retrieve resize parameters from the
    // DelegatedFrameHost and this view, so SynchronizeVisualProperties must be
    // called last.
    if (render_widget_host_) {
      render_widget_host_->SynchronizeVisualProperties();
    }
  }
}

void OffScreenRenderWidgetHostView::Invalidate() {
  GetCompositor()->ScheduleFullRedraw();
  compositor_->IssueBeginFrame();
}

void OffScreenRenderWidgetHostView::InvalidateRect(gfx::Rect const& rect) {
  GetCompositor()->ScheduleRedrawRect(rect);
  compositor_->IssueBeginFrame();
}

gfx::Size OffScreenRenderWidgetHostView::SizeInPixels() {
  if (IsPopupWidget()) {
    return gfx::ScaleToCeiledSize(popup_position_.size(),
                                  current_device_scale_factor_);
  } else {
    return gfx::ScaleToCeiledSize(size_,
                                  current_device_scale_factor_);
  }
}

void OffScreenRenderWidgetHostView::SendMouseEvent(
    const blink::WebMouseEvent& event) {
  for (auto* proxy_view : proxy_views_) {
    gfx::Rect bounds = proxy_view->GetBounds();
    if (bounds.Contains(event.PositionInWidget().x(),
                        event.PositionInWidget().y())) {
      blink::WebMouseEvent proxy_event(event);
      proxy_event.SetPositionInWidget(
          proxy_event.PositionInWidget().x() - bounds.x(),
          proxy_event.PositionInWidget().y() - bounds.y());

      ui::MouseEvent ui_event = UiMouseEventFromWebMouseEvent(proxy_event);
      proxy_view->OnEvent(&ui_event);
      return;
    }
  }

  if (!IsPopupWidget()) {
    if (popup_host_view_ &&
        popup_host_view_->popup_position_.Contains(
            event.PositionInWidget().x(), event.PositionInWidget().y())) {
      blink::WebMouseEvent popup_event(event);
      popup_event.SetPositionInWidget(
          popup_event.PositionInWidget().x() -
              popup_host_view_->popup_position_.x(),
          popup_event.PositionInWidget().y() -
              popup_host_view_->popup_position_.y());

      popup_host_view_->ProcessMouseEvent(popup_event, ui::LatencyInfo());
      return;
    }
  }

  if (render_widget_host_ && render_widget_host_->GetView()) {
    if (ShouldRouteEvents()) {
      render_widget_host_->delegate()->GetInputEventRouter()->RouteMouseEvent(
          this, const_cast<blink::WebMouseEvent*>(&event),
          ui::LatencyInfo(ui::SourceEventType::OTHER));
    } else {
      render_widget_host_->GetView()->ProcessMouseEvent(
          event, ui::LatencyInfo(ui::SourceEventType::OTHER));
    }
  }
}

void OffScreenRenderWidgetHostView::SendMouseWheelEvent(
    const blink::WebMouseWheelEvent& event) {
  for (auto* proxy_view : proxy_views_) {
    gfx::Rect bounds = proxy_view->GetBounds();
    if (bounds.Contains(event.PositionInWidget().x(),
                        event.PositionInWidget().y())) {
      blink::WebMouseWheelEvent proxy_event(event);
      proxy_event.SetPositionInWidget(
          proxy_event.PositionInWidget().x() - bounds.x(),
          proxy_event.PositionInWidget().y() - bounds.y());

      ui::MouseWheelEvent ui_event =
          UiMouseWheelEventFromWebMouseEvent(proxy_event);
      proxy_view->OnEvent(&ui_event);
      return;
    }
  }

  if (!IsPopupWidget() && popup_host_view_) {
    if (popup_host_view_->popup_position_.Contains(
            event.PositionInWidget().x(), event.PositionInWidget().y())) {
      blink::WebMouseWheelEvent popup_mouse_wheel_event(event);
      popup_mouse_wheel_event.SetPositionInWidget(
          event.PositionInWidget().x() - popup_host_view_->popup_position_.x(),
          event.PositionInWidget().y() - popup_host_view_->popup_position_.y());
      popup_mouse_wheel_event.SetPositionInScreen(
          popup_mouse_wheel_event.PositionInWidget().x(),
          popup_mouse_wheel_event.PositionInWidget().y());

      popup_host_view_->SendMouseWheelEvent(popup_mouse_wheel_event);
      return;
    } else {
      // Scrolling outside of the popup widget so destroy it.
      // Execute asynchronously to avoid deleting the widget from inside some
      // other callback.
      base::PostTask(
          FROM_HERE, {content::BrowserThread::UI},
          base::BindOnce(&OffScreenRenderWidgetHostView::CancelWidget,
                         popup_host_view_->weak_ptr_factory_.GetWeakPtr()));
    }
  }

  if (render_widget_host_ && render_widget_host_->GetView()) {
    blink::WebMouseWheelEvent& mouse_wheel_event =
        const_cast<blink::WebMouseWheelEvent&>(event);

    mouse_wheel_phase_handler_.SendWheelEndForTouchpadScrollingIfNeeded(false);
    mouse_wheel_phase_handler_.AddPhaseIfNeededAndScheduleEndEvent(
        mouse_wheel_event, false);

    if (ShouldRouteEvents()) {
      render_widget_host_->delegate()
          ->GetInputEventRouter()
          ->RouteMouseWheelEvent(
              this, const_cast<blink::WebMouseWheelEvent*>(&mouse_wheel_event),
              ui::LatencyInfo(ui::SourceEventType::WHEEL));
    } else {
      render_widget_host_->GetView()->ProcessMouseWheelEvent(
          mouse_wheel_event, ui::LatencyInfo(ui::SourceEventType::WHEEL));
    }
  }
}

bool OffScreenRenderWidgetHostView::ShouldRouteEvents() const {
  if (!render_widget_host_->delegate())
    return false;

  // Do not route events that are currently targeted to page popups such as
  // <select> element drop-downs, since these cannot contain cross-process
  // frames.
  if (!render_widget_host_->delegate()->IsWidgetForPrimaryMainFrame(
          render_widget_host_)) {
    return false;
  }

  return !!render_widget_host_->delegate()->GetInputEventRouter();
}

void OffScreenRenderWidgetHostView::OnPaint(const gfx::Rect& damage_rect,
                                            const SkBitmap& bitmap) {
  backing_ = std::make_unique<SkBitmap>();
  backing_->allocN32Pixels(bitmap.width(), bitmap.height(), !transparent_);
  bitmap.readPixels(backing_->pixmap());

  if (IsPopupWidget()) {
    if (parent_callback_) {
      parent_callback_.Run(this->popup_position_);
    } else {
      // Popup is not yet initialized, reset backing
      backing_ = std::make_unique<SkBitmap>();
    }
  } else {
    CompositeFrame(damage_rect);
  }
}

void OffScreenRenderWidgetHostView::OnPopupTexturePaint(
    const gpu::Mailbox& mailbox,
    const gpu::SyncToken& sync_token,
    const gfx::Rect& content_rect,
    const gfx::Rect& damage_rect,
    void (*callback)(void*, void*),
    void* context) {
  texture_callback_.Run(std::move(mailbox), std::move(sync_token),
                        std::move(content_rect), std::move(damage_rect),
                        true, callback, context);
}

void OffScreenRenderWidgetHostView::OnTexturePaint(
    const gpu::Mailbox& mailbox,
    const gpu::SyncToken& sync_token,
    const gfx::Rect& content_rect,
    const gfx::Rect& damage_rect,
    void (*callback)(void*, void*),
    void* context) {
  if (!painting_) {
    callback(context, new gpu::SyncToken());
    return;
  }

  if (!IsPopupWidget()) {
    texture_callback_.Run(std::move(mailbox), std::move(sync_token),
                          std::move(content_rect), std::move(damage_rect),
                          false, callback, context);
  } else if (parent_texture_callback_) {
    parent_texture_callback_.Run(
        std::move(mailbox), std::move(sync_token),
        gfx::Rect(popup_position_.origin(), content_rect.size()),
        std::move(damage_rect), callback, context);
  }

  // Release the resize hold when we reach the desired size.
  if (hold_resize_) {
    if (content_rect.size() == GetRootLayerPixelSize()) {

      ReleaseResizeHold();
    }
  }
}

void OffScreenRenderWidgetHostView::OnBackingTextureCreated(
    const gpu::Mailbox& mailbox) {
  ForceRenderFrames(20, TimeDeltaFromHz(5));
}

void DoForceRenderFrames(
    base::WeakPtr<OffScreenRenderWidgetHostView> view,
    int n,
    base::TimeDelta delay) {
  if (n-- > 0 && view) {
    view->Invalidate();
    base::PostDelayedTask(
        FROM_HERE, {content::BrowserThread::UI},
        base::BindOnce(&DoForceRenderFrames, view, n, delay), delay);
  }
}

void OffScreenRenderWidgetHostView::ForceRenderFrames(
    int n, base::TimeDelta delay) {
  DoForceRenderFrames(weak_ptr_factory_.GetWeakPtr(), n, delay);
}

void OffScreenRenderWidgetHostView::OnPopupPaint(const gfx::Rect& damage_rect) {
  CompositeFrame(gfx::ToEnclosingRect(
      ConvertRectToPixels(damage_rect, GetScaleFactor())));
}

void OffScreenRenderWidgetHostView::OnProxyViewPaint(
    const gfx::Rect& damage_rect) {
  auto* context_factory = content::GetContextFactory();
  auto context_provider = context_factory->SharedMainThreadContextProvider();
  auto* sii = context_provider->SharedImageInterface();

  SkBitmap frame;
  frame.allocN32Pixels(SizeInPixels().width(), SizeInPixels().height(), false);
  cc::SkiaPaintCanvas paint_canvas(frame);
  gfx::Canvas canvas(&paint_canvas, 1.0f);

  auto transform = gfx::Transform(
    1, 0, 0, 0,
    0, -1, 0, SizeInPixels().height(),
    0, 0, 1, 0,
    0, 0, 0, 1
  );

  canvas.Transform(transform);

  for (auto* proxy_view : proxy_views_) {
    gfx::Rect rect_in_pixels = proxy_view->GetBounds();

    if (!proxy_view->GetBitmap()->drawsNothing()) {
      gfx::ImageSkia image(gfx::ImageSkiaRep(*proxy_view->GetBitmap(), 1.0f));
      canvas.DrawImageInt(image,
                          rect_in_pixels.origin().x(),
                          rect_in_pixels.origin().y());
    }
  }

  void* pixel_data = frame.getPixels();
  auto pixel_size = frame.computeByteSize();

  base::span<const uint8_t> pixels =
      base::make_span(reinterpret_cast<const uint8_t*>(pixel_data), pixel_size);
  auto size = gfx::Size(frame.width(), frame.height());

  constexpr uint32_t kUsage = gpu::SHARED_IMAGE_USAGE_GLES2 |
                              gpu::SHARED_IMAGE_USAGE_GLES2_FRAMEBUFFER_HINT |
                              gpu::SHARED_IMAGE_USAGE_DISPLAY;

  auto mailbox = sii->CreateSharedImage(
      viz::ResourceFormat::RGBA_8888, size, gfx::ColorSpace(),
      kTopLeft_GrSurfaceOrigin, kPremul_SkAlphaType, kUsage, pixels);
  auto sync_token = sii->GenVerifiedSyncToken();

  struct MailboxHolder {
    gpu::Mailbox mailbox;
  };

  OnPopupTexturePaint(
      mailbox,
      sync_token,
      gfx::Rect(SizeInPixels()),
      gfx::Rect(SizeInPixels()),
      [] (void* context, void* token) {
        auto* context_factory = content::GetContextFactory();
        auto context_provider = context_factory->SharedMainThreadContextProvider();
        auto* sii = context_provider->SharedImageInterface();

        if (context) {
          sii->DestroySharedImage(
              *reinterpret_cast<gpu::SyncToken*>(token),
              reinterpret_cast<MailboxHolder*>(context)->mailbox);
        }
      },
      new MailboxHolder{std::move(mailbox)});
}

void OffScreenRenderWidgetHostView::CompositeFrame(
    const gfx::Rect& damage_rect) {
  gfx::Size size_in_pixels = SizeInPixels();
  gfx::Rect damage_rect_union = damage_rect;

  SkBitmap frame;

  // Optimize for the case when there is no popup
  if (proxy_views_.empty() && !popup_host_view_) {
    frame = GetBacking();
  } else {
    frame.allocN32Pixels(size_in_pixels.width(), size_in_pixels.height(),
                         false);
    if (!GetBacking().drawsNothing()) {
      SkCanvas canvas(frame);
      canvas.writePixels(GetBacking(), 0, 0);

      if (popup_host_view_ && !popup_host_view_->GetBacking().drawsNothing()) {
        damage_rect_union.Union(popup_position_);
        canvas.writePixels(popup_host_view_->GetBacking(),
                           popup_position_.origin().x(),
                           popup_position_.origin().y());
      }

      for (auto* proxy_view : proxy_views_) {
        gfx::Rect rect_in_pixels =
            gfx::ToEnclosingRect(ConvertRectToPixels(
                proxy_view->GetBounds(), GetScaleFactor()));
        damage_rect_union.Union(rect_in_pixels);
        canvas.writePixels(*proxy_view->GetBitmap(),
                           rect_in_pixels.origin().x(),
                           rect_in_pixels.origin().y());
      }
    }
  }

  gfx::Rect damage =
      gfx::IntersectRects(gfx::Rect(size_in_pixels), damage_rect_union);

  paint_callback_running_ = true;
  callback_.Run(damage, frame);
  paint_callback_running_ = false;
}

void OffScreenRenderWidgetHostView::CancelWidget() {
  if (render_widget_host_)
    render_widget_host_->LostCapture();
  Hide();

  if (parent_host_view_) {
    if (parent_host_view_->popup_host_view_ == this) {
      parent_texture_callback_.Run(gpu::Mailbox(), gpu::SyncToken(),
                                   gfx::Rect(), gfx::Rect(),
                                   nullptr, nullptr);

      parent_host_view_->set_popup_host_view(nullptr);
    } else if (parent_host_view_->child_host_view_ == this) {
      parent_host_view_->set_child_host_view(nullptr);
      parent_host_view_->Show();
    }
    parent_host_view_ = nullptr;
  }

  weak_ptr_factory_.InvalidateWeakPtrs();

  if (render_widget_host_ && !is_destroyed_) {
    is_destroyed_ = true;
    // Results in a call to Destroy().
    render_widget_host_->ShutdownAndDestroyWidget(true);
  }
}

void OffScreenRenderWidgetHostView::AddViewProxy(OffscreenViewProxy* proxy) {
  proxy->SetObserver(this);
  proxy_views_.insert(proxy);
}

void OffScreenRenderWidgetHostView::RemoveViewProxy(OffscreenViewProxy* proxy) {
  proxy->RemoveObserver();
  proxy_views_.erase(proxy);
}

void OffScreenRenderWidgetHostView::ProxyViewDestroyed(
    OffscreenViewProxy* proxy) {
  proxy_views_.erase(proxy);
  OnProxyViewPaint(gfx::Rect(size_));
}

void OffScreenRenderWidgetHostView::SetPainting(bool painting) {
  painting_ = painting;

  if (popup_host_view_) {
    popup_host_view_->SetPainting(painting);
  }

  if (compositor_) {
    compositor_->EnableBeginFrames(IsPainting());
  }

  if (video_consumer_) {
    video_consumer_->SetActive(IsPainting());
  } else if (host_display_client_) {
    host_display_client_->SetActive(IsPainting());
  }

  if (painting_) {
    Invalidate();
  }
}

bool OffScreenRenderWidgetHostView::IsPainting() const {
  return painting_;
}

void OffScreenRenderWidgetHostView::SetFrameRate(int frame_rate) {
  if (parent_host_view_) {
    if (parent_host_view_->GetFrameRate() == GetFrameRate())
      return;

    frame_rate_ = parent_host_view_->GetFrameRate();
  } else {
    if (frame_rate <= 0)
      frame_rate = 1;
    if (frame_rate > 240)
      frame_rate = 240;

    frame_rate_ = frame_rate;
  }

  SetupFrameRate();

  if (video_consumer_) {
    video_consumer_->SetFrameRate(GetFrameRate());
  }
}

int OffScreenRenderWidgetHostView::GetFrameRate() const {
  return frame_rate_;
}

bool OffScreenRenderWidgetHostView::UsingAutoScaleFactor() const {
  return manual_device_scale_factor_ == kAutoScaleFactor;
}

void OffScreenRenderWidgetHostView::SetManualScaleFactor(float scale_factor) {
  manual_device_scale_factor_ = scale_factor;
  SynchronizeVisualProperties(cc::DeadlinePolicy::UseExistingDeadline(),
                              absl::nullopt);
}

float OffScreenRenderWidgetHostView::GetScaleFactor() const {
  if (!UsingAutoScaleFactor())
    return manual_device_scale_factor_;

  return current_device_scale_factor_;
}

void OffScreenRenderWidgetHostView::OnDidUpdateVisualPropertiesComplete(
    const cc::RenderFrameMetadata& metadata) {
  if (host()->is_hidden()) {
    // When an embedded child responds, we want to accept its changes to the
    // viz::LocalSurfaceId. However we do not want to embed surfaces while
    // hidden. Nor do we want to embed invalid ids when we are evicted. Becoming
    // visible will generate a new id, if necessary, and begin embedding.
    UpdateLocalSurfaceIdFromEmbeddedClient(
        metadata.local_surface_id);
  } else {
    SynchronizeVisualProperties(cc::DeadlinePolicy::UseDefaultDeadline(),
                                metadata.local_surface_id);
  }
}

ui::Layer* OffScreenRenderWidgetHostView::GetRootLayer() const {
  return root_layer_.get();
}

gfx::Size OffScreenRenderWidgetHostView::GetRootLayerPixelSize() const {
  return GetRootLayer()->size();
}

const viz::LocalSurfaceId& OffScreenRenderWidgetHostView::GetLocalSurfaceId()
    const {
  return const_cast<OffScreenRenderWidgetHostView*>(this)
      ->GetOrCreateLocalSurfaceId();
}

content::DelegatedFrameHost*
OffScreenRenderWidgetHostView::GetDelegatedFrameHost() const {
  return delegated_frame_host_.get();
}

void OffScreenRenderWidgetHostView::SetupFrameRate() {
  if (compositor_) {
    compositor_->SetDisplayVSyncParameters(
        base::TimeTicks::Now(), TimeDeltaFromHz(frame_rate_));
  }
}


bool OffScreenRenderWidgetHostView::SetRootLayerSize(bool force) {
  display::Display display =
      display::Screen::GetScreen()->GetDisplayNearestView(GetNativeView());
  float scaleFactor = display.device_scale_factor();
  if (!UsingAutoScaleFactor()) {
    scaleFactor = manual_device_scale_factor_;
  }
  current_device_scale_factor_ = scaleFactor;

  gfx::Size size = GetViewBounds().size();

  const bool scale_factor_changed = (scaleFactor != GetScaleFactor());
  const bool view_bounds_changed = (size != GetRootLayer()->bounds().size());

  if (!force && !scale_factor_changed && !view_bounds_changed) {
    return false;
  }

  GetRootLayer()->SetBounds(gfx::Rect(size));

  if (compositor_) {
    compositor_local_surface_id_allocator_.GenerateId();
    compositor_->SetScaleAndSize(
        1.0, GetViewBounds().size(),
        compositor_local_surface_id_allocator_.GetCurrentLocalSurfaceId());
  }

  return (scale_factor_changed || view_bounds_changed);
}

bool OffScreenRenderWidgetHostView::ResizeRootLayer() {
  if (!hold_resize_) {
    // The resize hold is not currently active.
    if (SetRootLayerSize(false)) {
      // The size has changed. Avoid resizing again until ReleaseResizeHold() is
      // called.
      #if BUILDFLAG(IS_WIN)
        compositor_->DisableSwapUntilResize();
      #endif

      hold_resize_ = true;
      return true;
    }
  } else if (!pending_resize_) {
    // The resize hold is currently active. Another resize will be triggered
    // from ReleaseResizeHold().
    pending_resize_ = true;
  }

  return false;
}

void OffScreenRenderWidgetHostView::ReleaseResizeHold() {
  DCHECK(hold_resize_);
  hold_resize_ = false;

  #if BUILDFLAG(IS_WIN)
    compositor_->ReenableSwap();
  #endif

  if (pending_resize_) {
    pending_resize_ = false;

    base::PostTask(
        FROM_HERE, {content::BrowserThread::UI},
        base::BindOnce(&OffScreenRenderWidgetHostView::WasResized,
                       weak_ptr_factory_.GetWeakPtr()));
  }
}

viz::FrameSinkId OffScreenRenderWidgetHostView::AllocateFrameSinkId() {
  return render_widget_host_->GetFrameSinkId();
}

void OffScreenRenderWidgetHostView::AllocateLocalSurfaceId() {
  if (!parent_local_surface_id_allocator_) {
    parent_local_surface_id_allocator_ =
        std::make_unique<viz::ParentLocalSurfaceIdAllocator>();
  }
  parent_local_surface_id_allocator_->GenerateId();
}

const viz::LocalSurfaceId&
OffScreenRenderWidgetHostView::GetCurrentLocalSurfaceId() const {
  return parent_local_surface_id_allocator_->GetCurrentLocalSurfaceId();
}

void OffScreenRenderWidgetHostView::UpdateLocalSurfaceIdFromEmbeddedClient(
    const absl::optional<viz::LocalSurfaceId>&
        embedded_client_local_surface_id_allocation) {
  if (embedded_client_local_surface_id_allocation) {
    parent_local_surface_id_allocator_->UpdateFromChild(
        *embedded_client_local_surface_id_allocation);
  } else {
    AllocateLocalSurfaceId();
  }
}

const viz::LocalSurfaceId&
OffScreenRenderWidgetHostView::GetOrCreateLocalSurfaceId() {
  if (!parent_local_surface_id_allocator_)
    AllocateLocalSurfaceId();
  return GetCurrentLocalSurfaceId();
}

void OffScreenRenderWidgetHostView::InvalidateLocalSurfaceId() {
  if (!parent_local_surface_id_allocator_)
    return;
  parent_local_surface_id_allocator_->Invalidate();
}

void OffScreenRenderWidgetHostView::UpdateBackgroundColorFromRenderer(
    SkColor color) {
  if (color == background_color_)
    return;
  background_color_ = color;

  bool opaque = SkColorGetA(color) == SK_AlphaOPAQUE;
  GetRootLayer()->SetFillsBoundsOpaquely(opaque);
  GetRootLayer()->SetColor(color);
}

void OffScreenRenderWidgetHostView::NotifyHostAndDelegateOnWasShown(
    blink::mojom::RecordContentToVisibleTimeRequestPtr) {
  NOTREACHED();
}

void OffScreenRenderWidgetHostView::RequestPresentationTimeFromHostOrDelegate(
    blink::mojom::RecordContentToVisibleTimeRequestPtr) {
  NOTREACHED();
}

void OffScreenRenderWidgetHostView::
    CancelPresentationTimeRequestForHostAndDelegate() {
  NOTREACHED();
}

}  // namespace electron
