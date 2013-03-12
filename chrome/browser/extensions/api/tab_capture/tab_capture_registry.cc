// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/tab_capture/tab_capture_registry.h"

#include <utility>

#include "content/public/browser/browser_thread.h"
#include "chrome/browser/extensions/event_names.h"
#include "chrome/browser/extensions/event_router.h"
#include "chrome/browser/extensions/extension_system.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_dependency_manager.h"
#include "chrome/common/chrome_notification_types.h"
#include "chrome/common/extensions/extension.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_source.h"

namespace events = extensions::event_names;
using content::BrowserThread;

namespace extensions {

TabCaptureRegistry::TabCaptureRequest::TabCaptureRequest(
    const std::string& extension_id, const int tab_id,
    tab_capture::TabCaptureState status)
    : extension_id(extension_id), tab_id(tab_id), status(status),
      last_status(status) {
}

TabCaptureRegistry::TabCaptureRequest::~TabCaptureRequest() {
}

TabCaptureRegistry::TabCaptureRegistry(Profile* profile)
    : profile_(profile) {
  MediaCaptureDevicesDispatcher::GetInstance()->AddObserver(this);
  registrar_.Add(this, chrome::NOTIFICATION_EXTENSION_UNLOADED,
                 content::Source<Profile>(profile_));
}

TabCaptureRegistry::~TabCaptureRegistry() {
  MediaCaptureDevicesDispatcher::GetInstance()->RemoveObserver(this);
}

const TabCaptureRegistry::CaptureRequestList
    TabCaptureRegistry::GetCapturedTabs(const std::string& extension_id) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  CaptureRequestList list;
  for (DeviceCaptureRequestMap::const_iterator it = requests_.begin();
       it != requests_.end(); ++it) {
    if (it->second.extension_id == extension_id) {
      list.push_back(make_linked_ptr(new TabCaptureRequest(it->second)));
    }
  }
  return list;
}

void TabCaptureRegistry::Observe(int type,
                                 const content::NotificationSource& source,
                                 const content::NotificationDetails& details) {
  switch (type) {
    case chrome::NOTIFICATION_EXTENSION_UNLOADED: {
      // Cleanup all the requested media streams for this extension. We might
      // accumulate too many requests left in the closed state otherwise.
      std::string extension_id =
          content::Details<extensions::UnloadedExtensionInfo>(details)->
              extension->id();
      for (DeviceCaptureRequestMap::iterator it = requests_.begin();
           it != requests_.end();) {
        if (it->second.extension_id == extension_id) {
          requests_.erase(it++);
        } else {
          ++it;
        }
      }
      break;
    }
  }
}

bool TabCaptureRegistry::AddRequest(const std::pair<int, int> key,
                                    const TabCaptureRequest& request) {
  // Currently, we do not allow multiple active captures for same tab.
  DeviceCaptureRequestMap::iterator it = requests_.find(key);
  if (it != requests_.end()) {
    const tab_capture::TabCaptureState state = it->second.status;
    if (state != tab_capture::TAB_CAPTURE_STATE_STOPPED &&
        state != tab_capture::TAB_CAPTURE_STATE_ERROR)
      return false;
  }
  requests_.insert(std::make_pair(key, request));
  return true;
}

bool TabCaptureRegistry::VerifyRequest(int render_process_id,
                                       int render_view_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DVLOG(1) << "Verifying tabCapture request for "
           << render_process_id << ":" << render_view_id;
  return requests_.find(std::make_pair(
      render_process_id, render_view_id)) != requests_.end();
}

void TabCaptureRegistry::OnRequestUpdate(
    int render_process_id,
    int render_view_id,
    const content::MediaStreamDevice& device,
    const content::MediaRequestState new_state) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (device.type != content::MEDIA_TAB_VIDEO_CAPTURE &&
      device.type != content::MEDIA_TAB_AUDIO_CAPTURE)
    return;

  EventRouter* router = profile_ ?
      extensions::ExtensionSystem::Get(profile_)->event_router() : NULL;
  if (!router)
    return;

  std::pair<int, int> key = std::make_pair(render_process_id, render_view_id);
  DeviceCaptureRequestMap::iterator request_it = requests_.find(key);
  if (request_it == requests_.end()) {
    LOG(ERROR) << "Receiving updates for invalid tab capture request.";
    return;
  }

  tab_capture::TabCaptureState next_state = tab_capture::TAB_CAPTURE_STATE_NONE;
  switch (new_state) {
    case content::MEDIA_REQUEST_STATE_PENDING_APPROVAL:
      next_state = tab_capture::TAB_CAPTURE_STATE_PENDING;
      break;
    case content::MEDIA_REQUEST_STATE_DONE:
      next_state = tab_capture::TAB_CAPTURE_STATE_ACTIVE;
      break;
    case content::MEDIA_REQUEST_STATE_CLOSING:
      next_state = tab_capture::TAB_CAPTURE_STATE_STOPPED;
      break;
    case content::MEDIA_REQUEST_STATE_ERROR:
      next_state = tab_capture::TAB_CAPTURE_STATE_ERROR;
      break;
    case content::MEDIA_REQUEST_STATE_OPENING:
      return;
    case content::MEDIA_REQUEST_STATE_REQUESTED:
    case content::MEDIA_REQUEST_STATE_NOT_REQUESTED:
      NOTREACHED();
      return;
  }

  TabCaptureRegistry::TabCaptureRequest* request_info = &request_it->second;
  if (next_state == tab_capture::TAB_CAPTURE_STATE_PENDING &&
      request_info->status != tab_capture::TAB_CAPTURE_STATE_NONE &&
      request_info->status != tab_capture::TAB_CAPTURE_STATE_STOPPED &&
      request_info->status != tab_capture::TAB_CAPTURE_STATE_ERROR) {
    // If we end up trying to grab a new stream while the previous one was never
    // terminated, then something fishy is going on.
    LOG(ERROR) << "Trying to capture tab with existing stream.";
    return;
  }

  request_info->last_status = request_info->status;
  request_info->status = next_state;

  scoped_ptr<tab_capture::CaptureInfo> info(new tab_capture::CaptureInfo());
  info->tab_id = request_info->tab_id;
  info->status = request_info->status;

  // We will get duplicate events if we requested both audio and video, so only
  // send new events.
  if (request_info->last_status != request_info->status) {
    scoped_ptr<base::ListValue> args(new ListValue());
    args->Append(info->ToValue().release());
    scoped_ptr<Event> event(new Event(
        events::kOnTabCaptureStatusChanged, args.Pass()));
    event->restrict_to_profile = profile_;
    router->DispatchEventToExtension(request_info->extension_id, event.Pass());
  }
}

}  // namespace extensions
