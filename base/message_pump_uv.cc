// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/message_pump_uv.h"

#include "base/logging.h"
#include "base/command_line.h"
#include "content/public/common/content_switches.h"
#include "v8/include/v8.h"
#include "third_party/node/src/req_wrap.h"

namespace base {

namespace {

void wakeup_callback(uv_async_t* handle, int status) {
  // do nothing, just make libuv exit loop.
}

void idle_callback(uv_idle_t* handle, int status) {
  // do nothing, just make libuv exit loop.
}

void timer_callback(uv_timer_t* timer, int status) {
  // libuv would block unexpectedly with zero-timeout timer
  // this is a workaround of libuv bug #574:
  // https://github.com/joyent/libuv/issues/574
  uv_idle_start(static_cast<uv_idle_t*>(timer->data), idle_callback);
}

}  // namespace

MessagePumpUV::MessagePumpUV()
    : keep_running_(true),
      nesting_level_(0) {
  wakeup_events_.push(uv_async_t());
  uv_async_init(uv_default_loop(), &wakeup_events_.top(), wakeup_callback);
}

MessagePumpUV::~MessagePumpUV() {
}

void MessagePumpUV::Run(Delegate* delegate) {
  v8::HandleScope scope;

  ++nesting_level_;
  DCHECK(keep_running_) << "Quit must have been called outside of Run!";

  // Poll external loop in nested message loop, so node.js's events will be
  // paused in nested loop.
  uv_loop_t* loop = uv_default_loop();
  if (nesting_level_ > 1) {
    loop = uv_loop_new();

    wakeup_events_.push(uv_async_t());
    uv_async_init(loop, &wakeup_events_.top(), wakeup_callback);
  }

  // Create handles for the loop.
  uv_idle_t idle_handle;
  uv_idle_init(loop, &idle_handle);

  uv_timer_t delay_timer;
  delay_timer.data = &idle_handle;
  uv_timer_init(loop, &delay_timer);

  // Enter Loop
  for (;;) {
    bool did_work = delegate->DoWork();
    if (!keep_running_)
      break;

    did_work |= delegate->DoDelayedWork(&delayed_work_time_);
    if (!keep_running_)
      break;

    if (did_work)
      continue;

    did_work = delegate->DoIdleWork();
    if (!keep_running_)
      break;

    if (did_work)
      continue;

    // Enter node context while dealing with uv events.
    v8::Context::Scope context_scope(node::g_context);

    if (delayed_work_time_.is_null()) {
      uv_run_once(loop);
    } else {
      TimeDelta delay = delayed_work_time_ - TimeTicks::Now();
      if (delay > TimeDelta()) {
        uv_timer_start(&delay_timer, timer_callback,
                       delay.InMilliseconds(), 0);
        uv_run_once(loop);
        uv_idle_stop(&idle_handle);
        uv_timer_stop(&delay_timer);
      } else {
        // It looks like delayed_work_time_ indicates a time in the past, so we
        // need to call DoDelayedWork now.
        delayed_work_time_ = TimeTicks();
      }
    }
    // Since event_ is auto-reset, we don't need to do anything special here
    // other than service each delegate method.
  }

  if (nesting_level_ > 1) {
    // Delete external loop.
    uv_loop_delete(loop);

    // Restore previous async handle.
    wakeup_events_.pop();
  }

  keep_running_ = true;
  --nesting_level_;
}

void MessagePumpUV::Quit() {
  keep_running_ = false;
}

void MessagePumpUV::ScheduleWork() {
  // Since this can be called on any thread, we need to ensure that our Run
  // loop wakes up.
  uv_async_send(&wakeup_events_.top());
}

void MessagePumpUV::ScheduleDelayedWork(
    const TimeTicks& delayed_work_time) {
  // We know that we can't be blocked on Wait right now since this method can
  // only be called on the same thread as Run, so we only need to update our
  // record of how long to sleep when we do sleep.
  delayed_work_time_ = delayed_work_time;
}

}  // namespace base
