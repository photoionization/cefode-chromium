// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"
#include "content/shell/webkit_test_platform_support.h"

#include <AppKit/AppKit.h>
#include <Foundation/Foundation.h>

namespace content {

namespace {

void SetDefaultsToLayoutTestValues(void) {
  // So we can match the WebKit layout tests, we want to force a bunch of
  // preferences that control appearance to match.
  // (We want to do this as early as possible in application startup so
  // the settings are in before any higher layers could cache values.)

  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];

  const NSInteger kMinFontSizeCGSmoothes = 4;
  const NSInteger kNoFontSmoothing = 0;
  const NSInteger kBlueTintedAppearance = 1;
  [defaults setInteger:kMinFontSizeCGSmoothes
                forKey:@"AppleAntiAliasingThreshold"];
  [defaults setInteger:kNoFontSmoothing
                forKey:@"AppleFontSmoothing"];
  [defaults setInteger:kBlueTintedAppearance
                forKey:@"AppleAquaColorVariant"];
  [defaults setObject:@"0.709800 0.835300 1.000000"
               forKey:@"AppleHighlightColor"];
  [defaults setObject:@"0.500000 0.500000 0.500000"
               forKey:@"AppleOtherHighlightColor"];
  [defaults setObject:[NSArray arrayWithObject:@"en"]
               forKey:@"AppleLanguages"];
}

}  // namespace

bool WebKitTestPlatformInitialize() {

  SetDefaultsToLayoutTestValues();

  // Load font files in the resource folder.
  static const char* const fontFileNames[] = {
      "AHEM____.TTF",
      "WebKitWeightWatcher100.ttf",
      "WebKitWeightWatcher200.ttf",
      "WebKitWeightWatcher300.ttf",
      "WebKitWeightWatcher400.ttf",
      "WebKitWeightWatcher500.ttf",
      "WebKitWeightWatcher600.ttf",
      "WebKitWeightWatcher700.ttf",
      "WebKitWeightWatcher800.ttf",
      "WebKitWeightWatcher900.ttf",
  };

  // mainBundle is Content Shell Helper.app.  Go two levels up to find
  // Content Shell.app. Due to DumpRenderTree injecting the font files into
  // its direct dependents, it's not easily possible to put the ttf files into
  // the helper's resource directory instead of the outer bundle's resource
  // directory.
  NSString* bundle = [[NSBundle mainBundle] bundlePath];
  bundle = [bundle stringByAppendingPathComponent:@"../.."];
  NSURL* resources_directory = [[NSBundle bundleWithPath:bundle] resourceURL];

  NSMutableArray* font_urls = [NSMutableArray array];
  for (unsigned i = 0; i < arraysize(fontFileNames); ++i) {
    NSURL* font_url = [resources_directory
        URLByAppendingPathComponent:[NSString
            stringWithUTF8String:fontFileNames[i]]];
    [font_urls addObject:[font_url absoluteURL]];
  }

  CFArrayRef errors = 0;
  if (!CTFontManagerRegisterFontsForURLs((CFArrayRef)font_urls,
                                         kCTFontManagerScopeProcess,
                                         &errors)) {
    DLOG(FATAL) << "Fail to activate fonts.";
    CFRelease(errors);
  }

  return true;
}

}  // namespace
