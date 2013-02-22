// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/cefode_bindings.h"

#include "content/renderer/render_view_impl.h"
#include "third_party/node/src/req_wrap.h"
#include "third_party/node/src/node_javascript.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebDocument.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebFrame.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebScopedMicrotaskSuppression.h"
#include "v8/include/v8.h"

using WebKit::WebFrame;

namespace content {

extern GURL g_new_window_url;

void InjectCefodeBindings(WebFrame* frame) {
  v8::HandleScope handle_scope;

  v8::Handle<v8::Context> context = frame->mainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Context::Scope scope(context);

  // WebKit checks whether we're executing script outside ScriptController,
  // supress here.
  WebKit::WebScopedMicrotaskSuppression suppression;

  // Erase security token.
  context->SetSecurityToken(node::g_context->GetSecurityToken());

  // Inject node functions to DOM.
  v8::TryCatch try_catch;

  v8::Handle<v8::Script> script = node::CompileCefodeMainSource();
  v8::Local<v8::Value> result = script->Run();

  // Window opened by window.open will have blank URL at first, so check and
  // set the right URL here.
  GURL script_url = GURL(frame->document().url());
  if (script_url.spec() == "") {
    script_url = g_new_window_url;
    g_new_window_url = GURL();
  }

  std::string script_path = script_url.path();
  v8::Handle<v8::Value> args[2] = {
    v8::Local<v8::Value>::New(node::process),
    v8::String::New(script_path.c_str(), script_path.size())
  };
  v8::Local<v8::Function>::Cast(result)->Call(context->Global(), 2, args);
  if (try_catch.HasCaught()) {
    v8::String::Utf8Value trace(try_catch.StackTrace());
    fprintf(stderr, "%s\n", *trace);
  }
}

}  // namespace content

