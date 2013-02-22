// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_CEFODE_BINDINGS_H_
#define CONTENT_RENDERER_CEFODE_BINDINGS_H_

namespace WebKit {
class WebFrame;
}

namespace content {
void InjectCefodeBindings(WebKit::WebFrame* frame);
}  // namespace content

#endif  // CONTENT_RENDERER_CEFODE_BINDINGS_H_

