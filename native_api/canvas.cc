// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "electron/native_api/offscreen.h"

#include <map>

#include "base/logging.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/sync_token.h"

namespace {
electron::api::gpu::Mailbox ApiMailboxFromGpuMailbox(::gpu::Mailbox mailbox) {
  electron::api::gpu::Mailbox api_mailbox;

  memcpy(api_mailbox.name, mailbox.name, 16);
  api_mailbox.shared_image = mailbox.IsSharedImage();

  return api_mailbox;
}
}  // namespace

namespace electron {
namespace api {
namespace offscreen {

std::map<std::string, offscreen::Canvas::Observer*> Canvas::observers = {};

void Canvas::Producer::OnPaint(
    const char* uuid, const ::gpu::Mailbox& mailbox, 
    const ::gpu::SyncToken& sync_token,
    int x, int y, int width, int height, 
    void (*callback)(void*, void*), void* context) {
  const auto observer = Canvas::observers.find(uuid);

  if (observer != Canvas::observers.end()) {
    electron::api::gpu::SyncToken api_sync_token;
    api_sync_token.verified_flush = sync_token.verified_flush();
    api_sync_token.namespace_id =
        (electron::api::gpu::CommandBufferNamespace)sync_token.namespace_id();
    api_sync_token.command_buffer_id =
        sync_token.command_buffer_id().GetUnsafeValue();
    api_sync_token.release_count = sync_token.release_count();

    observer->second->OnCanvasTexturePaint(
        ApiMailboxFromGpuMailbox(mailbox), 
        api_sync_token,
        x, y, width, height, callback, context);
  } else {
    LOG(WARNING) << "OffscreenCanvas texture produced without observer for uuid = " << uuid;
  }
}

}  // namespace offscreen
}  // namespace api
}  // namespace electron
