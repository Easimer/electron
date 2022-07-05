// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_OSR_OSR_WEB_CONTENTS_VIEW_H_
#define ELECTRON_SHELL_BROWSER_OSR_OSR_WEB_CONTENTS_VIEW_H_

#include "shell/browser/api/electron_api_offscreen_window.h"

#include "content/browser/renderer_host/render_view_host_delegate_view.h"  // nogncheck
#include "content/browser/web_contents/web_contents_view.h"  // nogncheck
#include "content/public/browser/web_contents.h"
#include "shell/browser/osr/osr_render_widget_host_view.h"
#include "third_party/blink/public/common/page/drag_mojom_traits.h"

#if BUILDFLAG(IS_MAC)
#ifdef __OBJC__
@class OffScreenView;
#else
class OffScreenView;
#endif
#endif

namespace electron {

class OffScreenWebContentsView
    : public content::WebContentsView,
      public content::RenderViewHostDelegateView,
      public OffScreenRenderWidgetHostView::Initializer,
      public api::OffscreenWindow::Observer {
 public:
  OffScreenWebContentsView(bool transparent,
                           float scale_factor,
                           const OnPaintCallback& callback,
                           const OnTexturePaintCallback& texture_callback);
  ~OffScreenWebContentsView() override;

  void SetWebContents(content::WebContents*);
  void SetOffscreenWindow(api::OffscreenWindow* window);

  // api::OffscreenWindow::Observer:
  void OnWindowResize() override;
  void OnWindowClosed() override;

  // content::WebContentsView:
  gfx::NativeView GetNativeView() const override;
  gfx::NativeView GetContentNativeView() const override;
  gfx::NativeWindow GetTopLevelNativeWindow() const override;
  gfx::Rect GetContainerBounds() const override;
  void Focus() override;
  void SetInitialFocus() override;
  void StoreFocus() override;
  void RestoreFocus() override;
  void FocusThroughTabTraversal(bool reverse) override;
  content::DropData* GetDropData() const override;
  gfx::Rect GetViewBounds() const override;
  void CreateView(gfx::NativeView context) override;
  content::RenderWidgetHostViewBase* CreateViewForWidget(
      content::RenderWidgetHost* render_widget_host) override;
  content::RenderWidgetHostViewBase* CreateViewForChildWidget(
      content::RenderWidgetHost* render_widget_host) override;
  void SetPageTitle(const std::u16string& title) override;
  void RenderViewReady() override;
  void RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) override;
  void SetOverscrollControllerEnabled(bool enabled) override;
  void OnCapturerCountChanged() override;

#if BUILDFLAG(IS_MAC)
  bool CloseTabAfterEventTrackingIfNeeded() override;
#endif

  // content::RenderViewHostDelegateView
  void StartDragging(const content::DropData& drop_data,
                     blink::DragOperationsMask allowed_ops,
                     const gfx::ImageSkia& image,
                     const gfx::Vector2d& image_offset,
                     const blink::mojom::DragEventSourceInfo& event_info,
                     content::RenderWidgetHostImpl* source_rwh) override;
  void UpdateDragCursor(ui::mojom::DragOperation operation) override;

  // OffScreenRenderWidgetHostView::Initializer
  bool IsTransparent() const override;
  const OnPaintCallback& GetPaintCallback() const override;
  const OnTexturePaintCallback& GetTexturePaintCallback() const override;
  gfx::Size GetInitialSize() const override;

  void SetPainting(bool painting);
  bool IsPainting() const;
  void SetScaleFactor(float scale_factor);
  float GetScaleFactor() const;
  void SetFrameRate(int frame_rate);
  int GetFrameRate() const;

 private:
#if BUILDFLAG(IS_MAC)
  void PlatformCreate();
  void PlatformDestroy();
#endif

  OffScreenRenderWidgetHostView* GetView() const;

  api::OffscreenWindow* offscreen_window_;

  const bool transparent_;
  float scale_factor_;
  bool painting_ = true;
  int frame_rate_ = 120;
  OnPaintCallback callback_;
  OnTexturePaintCallback texture_callback_;

  // Weak refs.
  content::WebContents* web_contents_ = nullptr;

#if BUILDFLAG(IS_MAC)
  OffScreenView* offScreenView_;
#endif
};

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_OSR_OSR_WEB_CONTENTS_VIEW_H_
