// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/electron_autofill_driver.h"

#include <memory>

#include <utility>

#include "content/public/browser/render_widget_host_view.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/javascript_environment.h"
#include "shell/browser/native_window.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

namespace electron {

AutofillDriver::AutofillDriver(content::RenderFrameHost* render_frame_host)
    : render_frame_host_(render_frame_host) {
  render_frame_host_->GetRemoteAssociatedInterfaces()->GetInterface(
      &autofill_agent_);
  auto* web_contents = api::WebContents::From(
      content::WebContents::FromRenderFrameHost(render_frame_host));

  bool offscreen = false;
  if (web_contents && web_contents->owner_window()) {
    auto* embedder = web_contents->embedder();
    offscreen =
        web_contents->IsOffScreen() || (embedder && embedder->IsOffScreen());
  }

  autofill_popup_ = std::make_unique<AutofillPopup>(autofill_agent_, offscreen);
}  // namespace electron

AutofillDriver::~AutofillDriver() = default;

void AutofillDriver::BindPendingReceiver(
    mojo::PendingAssociatedReceiver<mojom::ElectronAutofillDriver>
        pending_receiver) {
  receiver_.Bind(std::move(pending_receiver));
}

void AutofillDriver::ShowAutofillPopup(
    const gfx::RectF& bounds,
    const std::vector<std::u16string>& values,
    const std::vector<std::u16string>& labels) {
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope(isolate);
  auto* web_contents = api::WebContents::From(
      content::WebContents::FromRenderFrameHost(render_frame_host_));
  if (!web_contents || !web_contents->owner_window())
    return;

  gfx::RectF popup_bounds(bounds);
  auto* embedder = web_contents->embedder();
  content::RenderFrameHost* embedder_frame_host = nullptr;
  if (embedder) {
    auto* embedder_view = embedder->web_contents()->GetMainFrame()->GetView();
    auto* view = web_contents->web_contents()->GetMainFrame()->GetView();
    auto offset = view->GetViewBounds().origin() -
                  embedder_view->GetViewBounds().origin();
    popup_bounds.Offset(offset);
    embedder_frame_host = embedder->web_contents()->GetMainFrame();
  }

  autofill_popup_->CreateView(render_frame_host_, embedder_frame_host,
                              web_contents->owner_window()->content_view(),
                              popup_bounds);
  autofill_popup_->SetItems(values, labels);
}

void AutofillDriver::HideAutofillPopup() {
  if (autofill_popup_)
    autofill_popup_->Hide();
}

}  // namespace electron
