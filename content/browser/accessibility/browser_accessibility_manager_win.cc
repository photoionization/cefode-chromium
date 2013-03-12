// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/accessibility/browser_accessibility_manager_win.h"

#include "content/browser/accessibility/browser_accessibility_win.h"
#include "content/common/accessibility_messages.h"

namespace content {

// static
BrowserAccessibilityManager* BrowserAccessibilityManager::Create(
    gfx::NativeView parent_view,
    const AccessibilityNodeData& src,
    BrowserAccessibilityDelegate* delegate,
    BrowserAccessibilityFactory* factory) {
  return new BrowserAccessibilityManagerWin(
      parent_view,
      src,
      delegate,
      factory);
}

BrowserAccessibilityManagerWin*
BrowserAccessibilityManager::ToBrowserAccessibilityManagerWin() {
  return static_cast<BrowserAccessibilityManagerWin*>(this);
}

BrowserAccessibilityManagerWin::BrowserAccessibilityManagerWin(
    HWND parent_view,
    const AccessibilityNodeData& src,
    BrowserAccessibilityDelegate* delegate,
    BrowserAccessibilityFactory* factory)
    : BrowserAccessibilityManager(parent_view, src, delegate, factory),
      tracked_scroll_object_(NULL) {
  // Allow NULL parent_view for unit testing.
  if (parent_view == NULL) {
    window_iaccessible_ = NULL;
    return;
  }

  HRESULT hr = ::CreateStdAccessibleObject(
      parent_view, OBJID_WINDOW, IID_IAccessible,
      reinterpret_cast<void **>(&window_iaccessible_));
  DCHECK(SUCCEEDED(hr));
}

BrowserAccessibilityManagerWin::~BrowserAccessibilityManagerWin() {
  if (tracked_scroll_object_) {
    tracked_scroll_object_->Release();
    tracked_scroll_object_ = NULL;
  }
}

IAccessible* BrowserAccessibilityManagerWin::GetParentWindowIAccessible() {
  return window_iaccessible_;
}

void BrowserAccessibilityManagerWin::NotifyAccessibilityEvent(
    int type,
    BrowserAccessibility* node) {
  LONG event_id = EVENT_MIN;
  switch (type) {
    case AccessibilityNotificationActiveDescendantChanged:
      event_id = IA2_EVENT_ACTIVE_DESCENDANT_CHANGED;
      break;
    case AccessibilityNotificationAlert:
      event_id = EVENT_SYSTEM_ALERT;
      break;
    case AccessibilityNotificationAriaAttributeChanged:
      event_id = IA2_EVENT_OBJECT_ATTRIBUTE_CHANGED;
      break;
    case AccessibilityNotificationAutocorrectionOccurred:
      event_id = IA2_EVENT_OBJECT_ATTRIBUTE_CHANGED;
      break;
    case AccessibilityNotificationBlur:
      // Equivalent to focus on the root.
      event_id = EVENT_OBJECT_FOCUS;
      node = GetRoot();
      break;
    case AccessibilityNotificationCheckStateChanged:
      event_id = EVENT_OBJECT_STATECHANGE;
      break;
    case AccessibilityNotificationChildrenChanged:
      event_id = EVENT_OBJECT_REORDER;
      break;
    case AccessibilityNotificationFocusChanged:
      event_id = EVENT_OBJECT_FOCUS;
      break;
    case AccessibilityNotificationInvalidStatusChanged:
      event_id = EVENT_OBJECT_STATECHANGE;
      break;
    case AccessibilityNotificationLiveRegionChanged:
      // TODO: try not firing a native notification at all, since
      // on Windows, each individual item in a live region that changes
      // already gets its own notification.
      event_id = EVENT_OBJECT_REORDER;
      break;
    case AccessibilityNotificationLoadComplete:
      event_id = IA2_EVENT_DOCUMENT_LOAD_COMPLETE;
      break;
    case AccessibilityNotificationMenuListItemSelected:
      event_id = EVENT_OBJECT_FOCUS;
      break;
    case AccessibilityNotificationMenuListValueChanged:
      event_id = EVENT_OBJECT_VALUECHANGE;
      break;
    case AccessibilityNotificationObjectHide:
      event_id = EVENT_OBJECT_HIDE;
      break;
    case AccessibilityNotificationObjectShow:
      event_id = EVENT_OBJECT_SHOW;
      break;
    case AccessibilityNotificationSelectedChildrenChanged:
      event_id = EVENT_OBJECT_SELECTIONWITHIN;
      break;
    case AccessibilityNotificationSelectedTextChanged:
      event_id = IA2_EVENT_TEXT_CARET_MOVED;
      break;
    case AccessibilityNotificationTextChanged:
      event_id = EVENT_OBJECT_NAMECHANGE;
      break;
    case AccessibilityNotificationTextInserted:
      event_id = IA2_EVENT_TEXT_INSERTED;
      break;
    case AccessibilityNotificationTextRemoved:
      event_id = IA2_EVENT_TEXT_REMOVED;
      break;
    case AccessibilityNotificationValueChanged:
      event_id = EVENT_OBJECT_VALUECHANGE;
      break;
    default:
      // Not all WebKit accessibility events result in a Windows
      // accessibility notification.
      break;
  }

  if (event_id != EVENT_MIN)
    NotifyWinEvent(event_id, GetParentView(), OBJID_CLIENT, node->child_id());

  // If this is a layout complete notification (sent when a container scrolls)
  // and there is a descendant tracked object, send a notification on it.
  // TODO(dmazzoni): remove once http://crbug.com/113483 is fixed.
  if (type == AccessibilityNotificationLayoutComplete &&
      tracked_scroll_object_ &&
      tracked_scroll_object_->IsDescendantOf(node)) {
    NotifyWinEvent(IA2_EVENT_VISIBLE_DATA_CHANGED,
                   GetParentView(),
                   OBJID_CLIENT,
                   tracked_scroll_object_->child_id());
    tracked_scroll_object_->Release();
    tracked_scroll_object_ = NULL;
  }
}

void BrowserAccessibilityManagerWin::TrackScrollingObject(
    BrowserAccessibilityWin* node) {
  if (tracked_scroll_object_)
    tracked_scroll_object_->Release();
  tracked_scroll_object_ = node;
  tracked_scroll_object_->AddRef();
}

}  // namespace content
