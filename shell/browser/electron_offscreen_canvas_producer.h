// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_RENDERER_ELECTRON_OFFSCREEN_CANVAS_PRODUCER_H_
#define ELECTRON_SHELL_RENDERER_ELECTRON_OFFSCREEN_CANVAS_PRODUCER_H_

#include <vector>

#include "base/memory/weak_ptr.h"
#include "electron/native_api/offscreen.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "electron/shell/common/api/api.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "ui/gfx/geometry/rect.h"

namespace electron {

class OffscreenCanvasProducer : public mojom::OffscreenCanvasTextureProducer,
                                public api::offscreen::Canvas::Producer {
 public:
  explicit OffscreenCanvasProducer(
        mojo::PendingReceiver<mojom::OffscreenCanvasTextureProducer> receiver);
  ~OffscreenCanvasProducer() override;

  // disable copy
  OffscreenCanvasProducer(const OffscreenCanvasProducer&) = delete;
  OffscreenCanvasProducer& operator=(const OffscreenCanvasProducer&) = delete;

  // mojom::OffscreenCanvasTextureProducer
  void OnTextureProduced(
      const std::string& uuid,
      const gpu::Mailbox& mailbox,
      const gpu::SyncToken& sync_token,
      const gfx::Rect& bounds,
      mojo::PendingRemote<viz::mojom::SingleReleaseCallback> callback) override;

  static void Create(
      mojo::PendingReceiver<mojom::OffscreenCanvasTextureProducer> receiver);

 private:
  mojo::Receiver<mojom::OffscreenCanvasTextureProducer> receiver_{this};

  base::WeakPtrFactory<OffscreenCanvasProducer> weak_ptr_factory_{this};
};

}  // namespace electron

#endif  // ELECTRON_SHELL_RENDERER_ELECTRON_OFFSCREEN_CANVAS_PRODUCER_H_
