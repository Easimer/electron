// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_OSR_OSR_RENDER_WIDGET_HOST_VIEW_H_
#define ELECTRON_SHELL_BROWSER_OSR_OSR_RENDER_WIDGET_HOST_VIEW_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

#include "base/callback_helpers.h"
#include "base/process/kill.h"
#include "base/task/single_thread_task_executor.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "content/browser/renderer_host/delegated_frame_host.h"  // nogncheck
#include "content/browser/renderer_host/input/mouse_wheel_phase_handler.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_impl.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_view_base.h"  // nogncheck
#include "content/browser/web_contents/web_contents_view.h"  // nogncheck
#include "gpu/command_buffer/common/mailbox_holder.h"
#include "shell/browser/osr/osr_host_display_client.h"
#include "shell/browser/osr/osr_video_consumer.h"
#include "shell/browser/osr/osr_view_proxy.h"
#include "third_party/blink/public/mojom/widget/record_content_to_visible_time_request.mojom-forward.h"
#include "third_party/blink/public/platform/web_vector.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer_owner.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/geometry/point.h"

#include "components/viz/host/host_display_client.h"

#if BUILDFLAG(IS_WIN)
#include "ui/gfx/win/window_impl.h"
#endif

namespace content {
class CursorManager;
}

namespace electron {

class ElectronDelegatedFrameHostClient;

typedef base::RepeatingCallback<void(const gfx::Rect&, const SkBitmap&)>
    OnPaintCallback;
typedef base::RepeatingCallback<void(const gpu::Mailbox&,
                                     const gpu::SyncToken&,
                                     const gfx::Rect&,
                                     const gfx::Rect&,
                                     bool,
                                     void (*)(void*, void*),
                                     void*)>
    OnTexturePaintCallback;
typedef base::RepeatingCallback<void(const gfx::Rect&)> OnPopupPaintCallback;
typedef base::RepeatingCallback<void(const gpu::Mailbox&,
                                     const gpu::SyncToken&,
                                     const gfx::Rect&,
                                     const gfx::Rect&,
                                     void (*)(void*, void*),
                                     void*)>
    OnPopupTexturePaintCallback;

class OffScreenRenderWidgetHostView : public content::RenderWidgetHostViewBase,
                                      public ui::CompositorDelegate,
                                      public OffscreenViewProxyObserver,
                                      public OffScreenHostDisplayClient::Observer {
 public:
  class Initializer {
   public:
    virtual bool IsTransparent() const = 0;
    virtual const OnPaintCallback& GetPaintCallback() const = 0;
    virtual const OnTexturePaintCallback& GetTexturePaintCallback() const = 0;
    virtual gfx::Size GetInitialSize() const = 0;
  };

  OffScreenRenderWidgetHostView(Initializer* initializer,
                                content::RenderWidgetHost* host,
                                OffScreenRenderWidgetHostView* parent,
                                bool painting,
                                int frame_rate,
                                float scale_factor);
  ~OffScreenRenderWidgetHostView() override;

  // disable copy
  OffScreenRenderWidgetHostView(const OffScreenRenderWidgetHostView&) = delete;
  OffScreenRenderWidgetHostView& operator=(
      const OffScreenRenderWidgetHostView&) = delete;

  // content::RenderWidgetHostView:
  void InitAsChild(gfx::NativeView) override;
  void SetSize(const gfx::Size&) override;
  void SetBounds(const gfx::Rect&) override;
  gfx::NativeView GetNativeView() override;
  gfx::NativeViewAccessible GetNativeViewAccessible() override;
  void Focus() override;
  bool HasFocus() override;
  uint32_t GetCaptureSequenceNumber() const override;
  bool IsSurfaceAvailableForCopy() override;
  void Hide() override;
  bool IsShowing() override;
  void EnsureSurfaceSynchronizedForWebTest() override;
  gfx::Rect GetViewBounds() override;
  void SetBackgroundColor(SkColor color) override;
  absl::optional<SkColor> GetBackgroundColor() override;
  void UpdateBackgroundColor() override;
  absl::optional<content::DisplayFeature> GetDisplayFeature() override;
  void SetDisplayFeatureForTesting(
      const content::DisplayFeature* display_feature) override;
  blink::mojom::PointerLockResult LockMouse(
      bool request_unadjusted_movement) override;
  blink::mojom::PointerLockResult ChangeMouseLock(
      bool request_unadjusted_movement) override;
  void UnlockMouse() override;
  void TakeFallbackContentFrom(content::RenderWidgetHostView* view) override;
#if BUILDFLAG(IS_MAC)
  void SetActive(bool active) override;
  void ShowDefinitionForSelection() override;
  void SpeakSelection() override;
  void SetWindowFrameInScreen(const gfx::Rect& rect) override;
  void ShowSharePicker(
      const std::string& title,
      const std::string& text,
      const std::string& url,
      const std::vector<std::string>& file_paths,
      blink::mojom::ShareService::ShareCallback callback) override;
  bool UpdateNSViewAndDisplay();
#endif  // BUILDFLAG(IS_MAC)

  // content::RenderWidgetHostViewBase:
  void ResetFallbackToFirstNavigationSurface() override;
  void InitAsPopup(content::RenderWidgetHostView* parent_host_view,
                   const gfx::Rect& pos,
                   const gfx::Rect& anchor_rect) override;
  void UpdateCursor(const content::WebCursor&) override;
  void SetIsLoading(bool is_loading) override;
  void RenderProcessGone() override;
  void ShowWithVisibility(content::PageVisibilityState page_visibility) final;
  void Destroy() override;
  void UpdateTooltipUnderCursor(const std::u16string&) override;
  content::CursorManager* GetCursorManager() override;

  void NotifyHostAndDelegateOnWasShown(
      blink::mojom::RecordContentToVisibleTimeRequestPtr) final;
  void RequestPresentationTimeFromHostOrDelegate(
      blink::mojom::RecordContentToVisibleTimeRequestPtr) final;
  void CancelPresentationTimeRequestForHostAndDelegate() final;
  gfx::Size GetCompositorViewportPixelSize() override;
  ui::Compositor* GetCompositor() override;

  content::RenderWidgetHostViewBase* CreateViewForWidget(
      content::RenderWidgetHost*,
      content::RenderWidgetHost*,
      content::WebContentsView*) override;

  const viz::LocalSurfaceId& GetLocalSurfaceId() const override;
  const viz::FrameSinkId& GetFrameSinkId() const override;

  void CopyFromSurface(
      const gfx::Rect& src_rect,
      const gfx::Size& output_size,
      base::OnceCallback<void(const SkBitmap&)> callback) override;
  display::ScreenInfos GetNewScreenInfosForUpdate() override;
  void TransformPointToRootSurface(gfx::PointF* point) override;
  gfx::Rect GetBoundsInRootWindow() override;
#if !defined(OS_MAC)
  viz::ScopedSurfaceIdAllocator DidUpdateVisualProperties(
      const cc::RenderFrameMetadata& metadata) override;
#endif

  viz::SurfaceId GetCurrentSurfaceId() const override;
  void ImeCompositionRangeChanged(const gfx::Range&,
                                  const std::vector<gfx::Rect>&) override;
  std::unique_ptr<content::SyntheticGestureTarget>
  CreateSyntheticGestureTarget() override;

  bool TransformPointToCoordSpaceForView(
      const gfx::PointF& point,
      RenderWidgetHostViewBase* target_view,
      gfx::PointF* transformed_point) override;

  void DidNavigate() override;
  viz::FrameSinkId GetRootFrameSinkId() override;

  // ui::CompositorDelegate:
  bool IsOffscreen() const override;
  std::unique_ptr<viz::HostDisplayClient> CreateHostDisplayClient(
      ui::Compositor* compositor) override;

  // OffScreenHostDisplayClient::Observer
  void OffScreenHostDisplayClientWillDelete() override;

  bool InstallTransparency();

  void WasResized();
  void SynchronizeVisualProperties(
      const cc::DeadlinePolicy& deadline_policy,
      const absl::optional<viz::LocalSurfaceId>& child_local_surface_id);
  void Invalidate();
  void InvalidateRect(gfx::Rect const& rect);
  gfx::Size SizeInPixels();

  void SendMouseEvent(const blink::WebMouseEvent& event);
  void SendMouseWheelEvent(const blink::WebMouseWheelEvent& event);
  bool ShouldRouteEvents() const;

  void OnPaint(const gfx::Rect& damage_rect, const SkBitmap& bitmap);
  void OnPopupTexturePaint(const gpu::Mailbox& mailbox,
                           const gpu::SyncToken& sync_token,
                           const gfx::Rect& content_rect,
                           const gfx::Rect& damage_rect,
                           void (*callback)(void*, void*),
                           void* context);
  void OnTexturePaint(const gpu::Mailbox& mailbox,
                      const gpu::SyncToken& sync_token,
                      const gfx::Rect& content_rect,
                      const gfx::Rect& damage_rect,
                      void (*callback)(void*, void*),
                      void* context);
  void OnBackingTextureCreated(const gpu::Mailbox& mailbox);
  void ForceRenderFrames(int n, base::TimeDelta delay);
  void OnPopupPaint(const gfx::Rect& damage_rect);
  void OnProxyViewPaint(const gfx::Rect& bounds) override;
  void CompositeFrame(const gfx::Rect& damage_rect);

  void CancelWidget();

  void AddViewProxy(OffscreenViewProxy* proxy);
  void RemoveViewProxy(OffscreenViewProxy* proxy);
  void ProxyViewDestroyed(OffscreenViewProxy* proxy) override;

  bool IsPopupWidget() const {
    return widget_type_ == content::WidgetType::kPopup;
  }

  const SkBitmap& GetBacking() { return *backing_.get(); }

  void SetPainting(bool painting);
  bool IsPainting() const;

  void SetFrameRate(int frame_rate);
  int GetFrameRate() const;

  bool UsingAutoScaleFactor() const;
  void SetManualScaleFactor(float scale_factor);
  float GetScaleFactor() const;

  void OnDidUpdateVisualPropertiesComplete(
      const cc::RenderFrameMetadata& metadata);

  ui::Layer* GetRootLayer() const;
  gfx::Size GetRootLayerPixelSize() const;

  content::DelegatedFrameHost* GetDelegatedFrameHost() const;

  content::RenderWidgetHostImpl* render_widget_host() const {
    return render_widget_host_;
  }

  gfx::Size size() const { return size_; }

  void set_popup_host_view(OffScreenRenderWidgetHostView* popup_view) {
    popup_host_view_ = popup_view;
  }

  void set_child_host_view(OffScreenRenderWidgetHostView* child_view) {
    child_host_view_ = child_view;
  }

  void InvalidateLocalSurfaceId();

 private:
  void SetupFrameRate();
  bool SetRootLayerSize(bool force);

  void AddGuestHostView(OffScreenRenderWidgetHostView* guest_host);
  void RemoveGuestHostView(OffScreenRenderWidgetHostView* guest_host);

  bool ResizeRootLayer();
  void ReleaseResizeHold();

  viz::FrameSinkId AllocateFrameSinkId();

  // Forces the view to allocate a new viz::LocalSurfaceId for the next
  // CompositorFrame submission in anticipation of a synchronization operation
  // that does not involve a resize or a device scale factor change.
  void AllocateLocalSurfaceId();
  const viz::LocalSurfaceId& GetCurrentLocalSurfaceId() const;

  // Sets the current viz::LocalSurfaceId, in cases where the embedded client
  // has allocated one. Also sets child sequence number component of the
  // viz::LocalSurfaceId allocator.
  void UpdateLocalSurfaceIdFromEmbeddedClient(
      const absl::optional<viz::LocalSurfaceId>& local_surface_id);

  // Returns the current viz::LocalSurfaceIdAllocation.
  const viz::LocalSurfaceId& GetOrCreateLocalSurfaceId();

  // Applies background color without notifying the RenderWidget about
  // opaqueness changes.
  void UpdateBackgroundColorFromRenderer(SkColor color);

  SkColor background_color_ = SkColor();

  int force_render_n_frames_ = 0;

  int frame_rate_ = 0;
  float manual_device_scale_factor_;
  float current_device_scale_factor_;

  std::unique_ptr<ui::Layer> root_layer_;
  std::unique_ptr<ui::Compositor> compositor_;
  std::unique_ptr<content::DelegatedFrameHost> delegated_frame_host_;
  std::unique_ptr<ElectronDelegatedFrameHostClient>
      delegated_frame_host_client_;

  // Used to allocate LocalSurfaceIds when this is embedding external content.
  std::unique_ptr<viz::ParentLocalSurfaceIdAllocator>
      parent_local_surface_id_allocator_;
  viz::ParentLocalSurfaceIdAllocator compositor_local_surface_id_allocator_;

  std::unique_ptr<content::CursorManager> cursor_manager_;

  OffScreenHostDisplayClient* host_display_client_;
  std::unique_ptr<OffScreenVideoConsumer> video_consumer_;

  bool hold_resize_ = false;
  bool hold_paint_ = false;
  bool pending_resize_ = false;
  base::OnceCallback<void()> last_frame_callback_;
  uint64_t last_frame_sequence_number_ =
      viz::BeginFrameArgs::kStartingFrameNumber;

  // The associated Model.  While |this| is being Destroyed,
  // |render_widget_host_| is NULL and the message loop is run one last time
  // Message handlers must check for a NULL |render_widget_host_|.
  content::RenderWidgetHostImpl* render_widget_host_;

  OffScreenRenderWidgetHostView* parent_host_view_ = nullptr;
  OffScreenRenderWidgetHostView* popup_host_view_ = nullptr;
  OffScreenRenderWidgetHostView* child_host_view_ = nullptr;
  std::set<OffscreenViewProxy*> proxy_views_;

  OnPaintCallback callback_;
  OnTexturePaintCallback texture_callback_;
  OnPopupPaintCallback parent_callback_;
  OnPopupTexturePaintCallback parent_texture_callback_;
  bool paint_callback_running_ = false;
  std::unique_ptr<SkBitmap> backing_;

  bool transparent_;
  bool painting_;
  bool is_showing_ = false;
  bool is_first_navigation_ = true;
  bool is_destroyed_ = false;
  bool layer_tree_frame_sink_initialized_ = false;
  bool skip_next_frame_ = false;

  gfx::Size size_;
  gfx::Rect popup_position_;
  gpu::MailboxHolder popup_;
  gfx::Rect popup_texture_rect_;

  content::MouseWheelPhaseHandler mouse_wheel_phase_handler_;

  // Latest capture sequence number which is incremented when the caller
  // requests surfaces be synchronized via
  // EnsureSurfaceSynchronizedForWebTest().
  uint32_t latest_capture_sequence_number_ = 0u;

  base::WeakPtrFactory<OffScreenRenderWidgetHostView> weak_ptr_factory_{this};
};

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_OSR_OSR_RENDER_WIDGET_HOST_VIEW_H_
