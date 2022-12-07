// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/electron_offscreen_canvas_producer.h"

#include <utility>
#include <vector>

#include "components/viz/common/resources/single_release_callback.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace electron {

OffscreenCanvasProducer::OffscreenCanvasProducer(
    mojo::PendingReceiver<mojom::OffscreenCanvasTextureProducer> receiver) {
  receiver_.Bind(std::move(receiver));
}

OffscreenCanvasProducer::~OffscreenCanvasProducer() = default;

void OffscreenCanvasProducer::OnTextureProduced(
    const std::string& uuid,
    const gpu::Mailbox& mailbox,
    const gpu::SyncToken& sync_token,
    const gfx::Rect& bounds,
    mojo::PendingRemote<viz::mojom::SingleReleaseCallback> callback) {
  struct FramePinner {
    mojo::PendingRemote<viz::mojom::SingleReleaseCallback> releaser;
  };

  OnPaint(
      uuid.c_str(),
      mailbox,
      sync_token,
      bounds.x(),
      bounds.y(),
      bounds.width(),
      bounds.height(),
      [](void* context, void* token) {
        FramePinner* pinner = static_cast<FramePinner*>(context);

        mojo::Remote<viz::mojom::SingleReleaseCallback> callback_remote(
            std::move(pinner->releaser));

        callback_remote->Run(gpu::SyncToken(), false);

        delete pinner;
      },
      new FramePinner{std::move(callback)});
}

void OffscreenCanvasProducer::Create(
    mojo::PendingReceiver<mojom::OffscreenCanvasTextureProducer> receiver) {
  new OffscreenCanvasProducer(std::move(receiver));
}

}  // namespace electron
