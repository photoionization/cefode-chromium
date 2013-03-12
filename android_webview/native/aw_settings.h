// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_WEBVIEW_NATIVE_AW_SETTINGS_H_
#define ANDROID_WEBVIEW_NATIVE_AW_SETTINGS_H_

#include <jni.h>

#include "base/android/jni_helper.h"
#include "base/android/scoped_java_ref.h"
#include "content/public/browser/web_contents_observer.h"

namespace android_webview {

class AwSettings : public content::WebContentsObserver {
 public:
  AwSettings(JNIEnv* env, jobject obj);
  virtual ~AwSettings();

  // Called from Java.
  void Destroy(JNIEnv* env, jobject obj);
  void SetTextZoom(JNIEnv* env, jobject obj, jint text_zoom_percent);
  void SetWebContents(JNIEnv* env, jobject obj, jint web_contents);

 private:
  void UpdateTextZoom();

  // WebContentsObserver overrides:
  virtual void RenderViewCreated(
      content::RenderViewHost* render_view_host) OVERRIDE;

  JavaObjectWeakGlobalRef java_ref_;
  int text_zoom_percent_;
};

bool RegisterAwSettings(JNIEnv* env);

}  // namespace android_webview

#endif  // ANDROID_WEBVIEW_NATIVE_AW_SETTINGS_H_
