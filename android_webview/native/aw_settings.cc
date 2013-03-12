// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/native/aw_settings.h"

#include "android_webview/browser/renderer_host/aw_render_view_host_ext.h"
#include "android_webview/native/aw_contents.h"
#include "jni/AwSettings_jni.h"
#include "webkit/glue/webkit_glue.h"

namespace android_webview {

AwSettings::AwSettings(JNIEnv* env, jobject obj)
    : java_ref_(env, obj),
      text_zoom_percent_(100) {
}

AwSettings::~AwSettings() {
}

void AwSettings::Destroy(JNIEnv* env, jobject obj) {
  delete this;
}

void AwSettings::SetTextZoom(JNIEnv* env, jobject obj, jint text_zoom_percent) {
  if (text_zoom_percent_ == text_zoom_percent) return;
  text_zoom_percent_ = text_zoom_percent;
  UpdateTextZoom();
}

void AwSettings::SetWebContents(JNIEnv* env, jobject obj, jint web_contents) {
  Observe(reinterpret_cast<content::WebContents*>(web_contents));
}

void AwSettings::UpdateTextZoom() {
  if (!web_contents()) return;
  AwContents* contents = AwContents::FromWebContents(web_contents());
  if (!contents) return;
  AwRenderViewHostExt* rvhe = contents->render_view_host_ext();
  if (!rvhe) return;
  if (text_zoom_percent_ > 0) {
    rvhe->SetTextZoomLevel(webkit_glue::ZoomFactorToZoomLevel(
        text_zoom_percent_ / 100.0f));
  } else {
    // Use the default zoom level value when Text Autosizer is turned on.
    rvhe->SetTextZoomLevel(0);
  }
}

void AwSettings::RenderViewCreated(content::RenderViewHost* render_view_host) {
  UpdateTextZoom();
}

static jint Init(JNIEnv* env,
                 jobject obj,
                 jint web_contents) {
  AwSettings* settings = new AwSettings(env, obj);
  settings->SetWebContents(env, obj, web_contents);
  return reinterpret_cast<jint>(settings);
}

bool RegisterAwSettings(JNIEnv* env) {
  return RegisterNativesImpl(env) >= 0;
}

}  // namespace android_webview
