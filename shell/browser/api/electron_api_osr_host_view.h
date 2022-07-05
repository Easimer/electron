// Copyright (c) 2018 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef SHELL_BROWSER_API_ELECTRON_API_OSR_HOST_VIEW_H_
#define SHELL_BROWSER_API_ELECTRON_API_OSR_HOST_VIEW_H_

#include "gin/handle.h"
#include "shell/browser/api/electron_api_view.h"
#include "shell/browser/api/electron_api_web_contents_view.h"

namespace electron {

namespace api {

class WebContents;

class OsrHostView : public View {
 public:
  static gin_helper::WrappableBase* New(gin::Arguments* args);

  static void BuildPrototype(v8::Isolate* isolate,
                             v8::Local<v8::FunctionTemplate> prototype);

  // disable copy
  OsrHostView(const OsrHostView&) = delete;
  OsrHostView& operator=(const OsrHostView&) = delete;
 protected:
  OsrHostView(v8::Isolate* isolate);
  ~OsrHostView() override;

  void AddChildWebContents(gin::Handle<WebContentsView> child);
  void RemoveChildWebContents(int32_t id);
  size_t ChildCount();

 private:
  std::vector<v8::Global<v8::Object>> child_views_;
};

}  // namespace api

}  // namespace electron

#endif  // SHELL_BROWSER_API_ELECTRON_API_OSR_HOST_VIEW_H_
