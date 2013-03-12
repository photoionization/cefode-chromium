// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if defined(OS_WIN)
#include <windows.h>
#endif

#include "content/common/gpu/gpu_channel.h"

#include <queue>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/debug/trace_event.h"
#include "base/message_loop_proxy.h"
#include "base/process_util.h"
#include "base/rand_util.h"
#include "base/string_util.h"
#include "base/timer.h"
#include "content/common/child_process.h"
#include "content/common/gpu/gpu_channel_manager.h"
#include "content/common/gpu/gpu_messages.h"
#include "content/common/gpu/sync_point_manager.h"
#include "content/public/common/content_switches.h"
#include "crypto/hmac.h"
#include "gpu/command_buffer/service/image_manager.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/command_buffer/service/gpu_scheduler.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_channel_proxy.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_image.h"
#include "ui/gl/gl_surface.h"

#if defined(OS_POSIX)
#include "ipc/ipc_channel_posix.h"
#endif

#if defined(OS_ANDROID)
#include "content/common/gpu/stream_texture_manager_android.h"
#endif

namespace content {
namespace {

// Number of milliseconds between successive vsync. Many GL commands block
// on vsync, so thresholds for preemption should be multiples of this.
const int64 kVsyncIntervalMs = 17;

// Amount of time that we will wait for an IPC to be processed before
// preempting. After a preemption, we must wait this long before triggering
// another preemption.
const int64 kPreemptWaitTimeMs = 2 * kVsyncIntervalMs;

// Once we trigger a preemption, the maximum duration that we will wait
// before clearing the preemption.
const int64 kMaxPreemptTimeMs = kVsyncIntervalMs;

// Stop the preemption once the time for the longest pending IPC drops
// below this threshold.
const int64 kStopPreemptThresholdMs = kVsyncIntervalMs;

// Generates mailbox names for clients of the GPU process on the IO thread.
class MailboxMessageFilter : public IPC::ChannelProxy::MessageFilter {
 public:
  explicit MailboxMessageFilter(const std::string& private_key)
      : channel_(NULL),
        hmac_(crypto::HMAC::SHA256) {
    bool success = hmac_.Init(base::StringPiece(private_key));
    DCHECK(success);
  }

  virtual void OnFilterAdded(IPC::Channel* channel) OVERRIDE {
    DCHECK(!channel_);
    channel_ = channel;
  }

  virtual void OnFilterRemoved() OVERRIDE {
    DCHECK(channel_);
    channel_ = NULL;
  }

  virtual bool OnMessageReceived(const IPC::Message& message) OVERRIDE {
    DCHECK(channel_);

    bool handled = true;
    IPC_BEGIN_MESSAGE_MAP(MailboxMessageFilter, message)
      IPC_MESSAGE_HANDLER(GpuChannelMsg_GenerateMailboxNames,
                          OnGenerateMailboxNames)
      IPC_MESSAGE_HANDLER(GpuChannelMsg_GenerateMailboxNamesAsync,
                          OnGenerateMailboxNamesAsync)
      IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()

    return handled;
  }

  bool Send(IPC::Message* message) {
    return channel_->Send(message);
  }

 private:
  virtual ~MailboxMessageFilter() {
  }

  // Message handlers.
  void OnGenerateMailboxNames(unsigned num, std::vector<std::string>* result) {
    TRACE_EVENT1("gpu", "OnGenerateMailboxNames", "num", num);

    result->resize(num);

    for (unsigned i = 0; i < num; ++i) {
      char name[GL_MAILBOX_SIZE_CHROMIUM];
      base::RandBytes(name, sizeof(name) / 2);

      bool success = hmac_.Sign(
          base::StringPiece(name, sizeof(name) / 2),
          reinterpret_cast<unsigned char*>(name) + sizeof(name) / 2,
          sizeof(name) / 2);
      DCHECK(success);

      (*result)[i].assign(name, sizeof(name));
    }
  }

  void OnGenerateMailboxNamesAsync(unsigned num) {
    std::vector<std::string> names;
    OnGenerateMailboxNames(num, &names);
    Send(new GpuChannelMsg_GenerateMailboxNamesReply(names));
  }

  IPC::Channel* channel_;
  crypto::HMAC hmac_;
};
}  // anonymous namespace

// This filter does two things:
// - it counts and timestamps each message coming in on the channel
//   so that we can preempt other channels if a message takes too long to
//   process. To guarantee fairness, we must wait a minimum amount of time
//   before preempting and we limit the amount of time that we can preempt in
//   one shot (see constants above).
// - it handles the GpuCommandBufferMsg_InsertSyncPoint message on the IO
//   thread, generating the sync point ID and responding immediately, and then
//   posting a task to insert the GpuCommandBufferMsg_RetireSyncPoint message
//   into the channel's queue.
class SyncPointMessageFilter : public IPC::ChannelProxy::MessageFilter {
 public:
  // Takes ownership of gpu_channel (see below).
  SyncPointMessageFilter(base::WeakPtr<GpuChannel>* gpu_channel,
                         scoped_refptr<SyncPointManager> sync_point_manager,
                         scoped_refptr<base::MessageLoopProxy> message_loop)
      : preemption_state_(IDLE),
        gpu_channel_(gpu_channel),
        channel_(NULL),
        sync_point_manager_(sync_point_manager),
        message_loop_(message_loop),
        messages_received_(0) {
  }

  virtual void OnFilterAdded(IPC::Channel* channel) OVERRIDE {
    DCHECK(!channel_);
    channel_ = channel;
  }

  virtual void OnFilterRemoved() OVERRIDE {
    DCHECK(channel_);
    channel_ = NULL;
  }

  virtual bool OnMessageReceived(const IPC::Message& message) OVERRIDE {
    DCHECK(channel_);
    if (message.type() == GpuCommandBufferMsg_RetireSyncPoint::ID) {
      // This message should not be sent explicitly by the renderer.
      NOTREACHED();
      return true;
    }

    messages_received_++;
    if (preempting_flag_.get())
      pending_messages_.push(PendingMessage(messages_received_));
    UpdatePreemptionState();

    if (message.type() == GpuCommandBufferMsg_InsertSyncPoint::ID) {
      uint32 sync_point = sync_point_manager_->GenerateSyncPoint();
      IPC::Message* reply = IPC::SyncMessage::GenerateReply(&message);
      GpuCommandBufferMsg_InsertSyncPoint::WriteReplyParams(reply, sync_point);
      channel_->Send(reply);
      message_loop_->PostTask(FROM_HERE, base::Bind(
          &SyncPointMessageFilter::InsertSyncPointOnMainThread,
          gpu_channel_,
          sync_point_manager_,
          message.routing_id(),
          sync_point));
      return true;
    } else {
      return false;
    }
  }

  void MessageProcessed(uint64 messages_processed) {
    while (!pending_messages_.empty() &&
           pending_messages_.front().message_number <= messages_processed)
      pending_messages_.pop();
    UpdatePreemptionState();
  }

  void SetPreemptingFlag(gpu::PreemptionFlag* preempting_flag) {
    preempting_flag_ = preempting_flag;
  }

 protected:
  virtual ~SyncPointMessageFilter() {
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &SyncPointMessageFilter::DeleteWeakPtrOnMainThread, gpu_channel_));
  }

 private:
  enum PreemptionState {
    // Either there's no other channel to preempt, there are no messages
    // pending processing, or we just finished preempting and have to wait
    // before preempting again.
    IDLE,
    // We are waiting kPreemptWaitTimeMs before checking if we should preempt.
    WAITING,
    // We can preempt whenever any IPC processing takes more than
    // kPreemptWaitTimeMs.
    CHECKING,
    // We are currently preempting.
    PREEMPTING,
  };

  PreemptionState preemption_state_;

  struct PendingMessage {
    uint64 message_number;
    base::TimeTicks time_received;

    explicit PendingMessage(uint64 message_number)
        : message_number(message_number),
          time_received(base::TimeTicks::Now()) {
    }
  };

  void UpdatePreemptionState() {
    switch (preemption_state_) {
      case IDLE:
        if (preempting_flag_.get() && !pending_messages_.empty())
          TransitionToWaiting();
        break;
      case WAITING:
        // A timer will transition us to CHECKING.
        DCHECK(timer_.IsRunning());
        break;
      case CHECKING:
        if (!pending_messages_.empty()) {
          base::TimeDelta time_elapsed =
              base::TimeTicks::Now() - pending_messages_.front().time_received;
          if (time_elapsed.InMilliseconds() < kPreemptWaitTimeMs) {
            // Schedule another check for when the IPC may go long.
            timer_.Start(
                FROM_HERE,
                base::TimeDelta::FromMilliseconds(kPreemptWaitTimeMs) -
                    time_elapsed,
                this, &SyncPointMessageFilter::UpdatePreemptionState);
          } else {
            TransitionToPreempting();
          }
        }
        break;
      case PREEMPTING:
        if (pending_messages_.empty()) {
          TransitionToIdle();
        } else {
          base::TimeDelta time_elapsed =
              base::TimeTicks::Now() - pending_messages_.front().time_received;
          if (time_elapsed.InMilliseconds() < kStopPreemptThresholdMs)
            TransitionToIdle();
        }
        break;
      default:
        NOTREACHED();
    }
  }

  void TransitionToIdle() {
    DCHECK_EQ(preemption_state_, PREEMPTING);
    // Stop any outstanding timer set to force us from PREEMPTING to IDLE.
    timer_.Stop();

    preemption_state_ = IDLE;
    preempting_flag_->Reset();
    TRACE_COUNTER_ID1("gpu", "GpuChannel::Preempting", this, 0);

    UpdatePreemptionState();
  }

  void TransitionToWaiting() {
    DCHECK_EQ(preemption_state_, IDLE);
    DCHECK(!timer_.IsRunning());

    preemption_state_ = WAITING;
    timer_.Start(
        FROM_HERE,
        base::TimeDelta::FromMilliseconds(kPreemptWaitTimeMs),
        this, &SyncPointMessageFilter::TransitionToChecking);
  }

  void TransitionToChecking() {
    DCHECK_EQ(preemption_state_, WAITING);
    DCHECK(!timer_.IsRunning());

    preemption_state_ = CHECKING;
    UpdatePreemptionState();
  }

  void TransitionToPreempting() {
    DCHECK_EQ(preemption_state_, CHECKING);

    // Stop any pending state update checks that we may have queued
    // while CHECKING.
    timer_.Stop();

    preemption_state_ = PREEMPTING;
    preempting_flag_->Set();
    TRACE_COUNTER_ID1("gpu", "GpuChannel::Preempting", this, 1);

    timer_.Start(
       FROM_HERE,
       base::TimeDelta::FromMilliseconds(kMaxPreemptTimeMs),
       this, &SyncPointMessageFilter::TransitionToIdle);

    UpdatePreemptionState();
  }

  static void InsertSyncPointOnMainThread(
      base::WeakPtr<GpuChannel>* gpu_channel,
      scoped_refptr<SyncPointManager> manager,
      int32 routing_id,
      uint32 sync_point) {
    // This function must ensure that the sync point will be retired. Normally
    // we'll find the stub based on the routing ID, and associate the sync point
    // with it, but if that fails for any reason (channel or stub already
    // deleted, invalid routing id), we need to retire the sync point
    // immediately.
    if (gpu_channel->get()) {
      GpuCommandBufferStub* stub = gpu_channel->get()->LookupCommandBuffer(
          routing_id);
      if (stub) {
        stub->AddSyncPoint(sync_point);
        GpuCommandBufferMsg_RetireSyncPoint message(routing_id, sync_point);
        gpu_channel->get()->OnMessageReceived(message);
        return;
      } else {
        gpu_channel->get()->MessageProcessed();
      }
    }
    manager->RetireSyncPoint(sync_point);
  }

  static void DeleteWeakPtrOnMainThread(
      base::WeakPtr<GpuChannel>* gpu_channel) {
    delete gpu_channel;
  }

  // NOTE: this is a pointer to a weak pointer. It is never dereferenced on the
  // IO thread, it's only passed through - therefore the WeakPtr assumptions are
  // respected.
  base::WeakPtr<GpuChannel>* gpu_channel_;
  IPC::Channel* channel_;
  scoped_refptr<SyncPointManager> sync_point_manager_;
  scoped_refptr<base::MessageLoopProxy> message_loop_;
  scoped_refptr<gpu::PreemptionFlag> preempting_flag_;

  std::queue<PendingMessage> pending_messages_;

  // Count of the number of IPCs received on this GpuChannel.
  uint64 messages_received_;

  base::OneShotTimer<SyncPointMessageFilter> timer_;
};

GpuChannel::GpuChannel(GpuChannelManager* gpu_channel_manager,
                       GpuWatchdog* watchdog,
                       gfx::GLShareGroup* share_group,
                       gpu::gles2::MailboxManager* mailbox,
                       int client_id,
                       bool software)
    : gpu_channel_manager_(gpu_channel_manager),
      messages_processed_(0),
      client_id_(client_id),
      share_group_(share_group ? share_group : new gfx::GLShareGroup),
      mailbox_manager_(mailbox ? mailbox : new gpu::gles2::MailboxManager),
      image_manager_(new gpu::gles2::ImageManager),
      watchdog_(watchdog),
      software_(software),
      handle_messages_scheduled_(false),
      processed_get_state_fast_(false),
      currently_processing_message_(NULL),
      weak_factory_(ALLOW_THIS_IN_INITIALIZER_LIST(this)) {
  DCHECK(gpu_channel_manager);
  DCHECK(client_id);

  channel_id_ = IPC::Channel::GenerateVerifiedChannelID("gpu");
  const CommandLine* command_line = CommandLine::ForCurrentProcess();
  log_messages_ = command_line->HasSwitch(switches::kLogPluginMessages);
  disallowed_features_.multisampling =
      command_line->HasSwitch(switches::kDisableGLMultisampling);
#if defined(OS_ANDROID)
  stream_texture_manager_.reset(new StreamTextureManagerAndroid(this));
#endif
}


bool GpuChannel::Init(base::MessageLoopProxy* io_message_loop,
                      base::WaitableEvent* shutdown_event) {
  DCHECK(!channel_.get());

  // Map renderer ID to a (single) channel to that process.
  channel_.reset(new IPC::SyncChannel(
      channel_id_,
      IPC::Channel::MODE_SERVER,
      this,
      io_message_loop,
      false,
      shutdown_event));

  base::WeakPtr<GpuChannel>* weak_ptr(new base::WeakPtr<GpuChannel>(
      weak_factory_.GetWeakPtr()));

  // Add the MailboxMessageFilter first so that SyncPointMessageFilter
  // does not count IPCs handled by the MailboxMessageFilter.
  channel_->AddFilter(
      new MailboxMessageFilter(mailbox_manager_->private_key()));

  filter_ = new SyncPointMessageFilter(
      weak_ptr,
      gpu_channel_manager_->sync_point_manager(),
      base::MessageLoopProxy::current());
  io_message_loop_ = io_message_loop;
  channel_->AddFilter(filter_);

  return true;
}

std::string GpuChannel::GetChannelName() {
  return channel_id_;
}

#if defined(OS_POSIX)
int GpuChannel::TakeRendererFileDescriptor() {
  if (!channel_.get()) {
    NOTREACHED();
    return -1;
  }
  return channel_->TakeClientFileDescriptor();
}
#endif  // defined(OS_POSIX)

bool GpuChannel::OnMessageReceived(const IPC::Message& message) {
  bool message_processed = true;
  if (log_messages_) {
    DVLOG(1) << "received message @" << &message << " on channel @" << this
             << " with type " << message.type();
  }

  if (message.type() == GpuCommandBufferMsg_GetStateFast::ID) {
    if (processed_get_state_fast_) {
      // Require a non-GetStateFast message in between two GetStateFast
      // messages, to ensure progress is made.
      std::deque<IPC::Message*>::iterator point = deferred_messages_.begin();

      while (point != deferred_messages_.end() &&
             (*point)->type() == GpuCommandBufferMsg_GetStateFast::ID) {
        ++point;
      }

      if (point != deferred_messages_.end()) {
        ++point;
      }

      deferred_messages_.insert(point, new IPC::Message(message));
      message_processed = false;
    } else {
      // Move GetStateFast commands to the head of the queue, so the renderer
      // doesn't have to wait any longer than necessary.
      deferred_messages_.push_front(new IPC::Message(message));
      message_processed = false;
    }
  } else {
    deferred_messages_.push_back(new IPC::Message(message));
    message_processed = false;
  }

  if (message_processed)
    MessageProcessed();

  OnScheduled();

  return true;
}

void GpuChannel::OnChannelError() {
  gpu_channel_manager_->RemoveChannel(client_id_);
}

bool GpuChannel::Send(IPC::Message* message) {
  // The GPU process must never send a synchronous IPC message to the renderer
  // process. This could result in deadlock.
  DCHECK(!message->is_sync());
  if (log_messages_) {
    DVLOG(1) << "sending message @" << message << " on channel @" << this
             << " with type " << message->type();
  }

  if (!channel_.get()) {
    delete message;
    return false;
  }

  return channel_->Send(message);
}

void GpuChannel::RequeueMessage() {
  DCHECK(currently_processing_message_);
  deferred_messages_.push_front(
      new IPC::Message(*currently_processing_message_));
  messages_processed_--;
  currently_processing_message_ = NULL;
}

void GpuChannel::OnScheduled() {
  if (handle_messages_scheduled_)
    return;
  // Post a task to handle any deferred messages. The deferred message queue is
  // not emptied here, which ensures that OnMessageReceived will continue to
  // defer newly received messages until the ones in the queue have all been
  // handled by HandleMessage. HandleMessage is invoked as a
  // task to prevent reentrancy.
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&GpuChannel::HandleMessage, weak_factory_.GetWeakPtr()));
  handle_messages_scheduled_ = true;
}

void GpuChannel::CreateViewCommandBuffer(
    const gfx::GLSurfaceHandle& window,
    int32 surface_id,
    const GPUCreateCommandBufferConfig& init_params,
    int32* route_id) {
  TRACE_EVENT1("gpu",
               "GpuChannel::CreateViewCommandBuffer",
               "surface_id",
               surface_id);

  *route_id = MSG_ROUTING_NONE;

#if defined(ENABLE_GPU)

  GpuCommandBufferStub* share_group = stubs_.Lookup(init_params.share_group_id);

  *route_id = GenerateRouteID();
  scoped_ptr<GpuCommandBufferStub> stub(new GpuCommandBufferStub(
      this,
      share_group,
      window,
      mailbox_manager_,
      image_manager_,
      gfx::Size(),
      disallowed_features_,
      init_params.allowed_extensions,
      init_params.attribs,
      init_params.gpu_preference,
      *route_id,
      surface_id,
      watchdog_,
      software_,
      init_params.active_url));
  if (preempted_flag_.get())
    stub->SetPreemptByFlag(preempted_flag_);
  router_.AddRoute(*route_id, stub.get());
  stubs_.AddWithID(stub.release(), *route_id);
#endif  // ENABLE_GPU
}

GpuCommandBufferStub* GpuChannel::LookupCommandBuffer(int32 route_id) {
  return stubs_.Lookup(route_id);
}

void GpuChannel::CreateImage(
    gfx::PluginWindowHandle window,
    int32 image_id,
    gfx::Size* size) {
  TRACE_EVENT1("gpu",
               "GpuChannel::CreateImage",
               "image_id",
               image_id);

  *size = gfx::Size();

  if (image_manager_->LookupImage(image_id)) {
    LOG(ERROR) << "CreateImage failed, image_id already in use.";
    return;
  }

  scoped_refptr<gfx::GLImage> image = gfx::GLImage::CreateGLImage(window);
  if (!image)
    return;

  image_manager_->AddImage(image.get(), image_id);
  *size = image->GetSize();
}

void GpuChannel::DeleteImage(int32 image_id) {
  TRACE_EVENT1("gpu",
               "GpuChannel::DeleteImage",
               "image_id",
               image_id);

  image_manager_->RemoveImage(image_id);
}

void GpuChannel::LoseAllContexts() {
  gpu_channel_manager_->LoseAllContexts();
}

void GpuChannel::DestroySoon() {
  MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(&GpuChannel::OnDestroy, this));
}

int GpuChannel::GenerateRouteID() {
  static int last_id = 0;
  return ++last_id;
}

void GpuChannel::AddRoute(int32 route_id, IPC::Listener* listener) {
  router_.AddRoute(route_id, listener);
}

void GpuChannel::RemoveRoute(int32 route_id) {
  router_.RemoveRoute(route_id);
}

gpu::PreemptionFlag* GpuChannel::GetPreemptionFlag() {
  if (!preempting_flag_.get()) {
    preempting_flag_ = new gpu::PreemptionFlag;
    io_message_loop_->PostTask(
        FROM_HERE,
        base::Bind(&SyncPointMessageFilter::SetPreemptingFlag,
                   filter_, preempting_flag_));
  }
  return preempting_flag_.get();
}

void GpuChannel::SetPreemptByFlag(
    scoped_refptr<gpu::PreemptionFlag> preempted_flag) {
  preempted_flag_ = preempted_flag;

  for (StubMap::Iterator<GpuCommandBufferStub> it(&stubs_);
       !it.IsAtEnd(); it.Advance()) {
    it.GetCurrentValue()->SetPreemptByFlag(preempted_flag_);
  }
}

GpuChannel::~GpuChannel() {
  if (preempting_flag_.get())
    preempting_flag_->Reset();
}

void GpuChannel::OnDestroy() {
  TRACE_EVENT0("gpu", "GpuChannel::OnDestroy");
  gpu_channel_manager_->RemoveChannel(client_id_);
}

bool GpuChannel::OnControlMessageReceived(const IPC::Message& msg) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(GpuChannel, msg)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_CreateOffscreenCommandBuffer,
                                    OnCreateOffscreenCommandBuffer)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_DestroyCommandBuffer,
                                    OnDestroyCommandBuffer)
#if defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_RegisterStreamTextureProxy,
                        OnRegisterStreamTextureProxy)
    IPC_MESSAGE_HANDLER(GpuChannelMsg_EstablishStreamTexture,
                        OnEstablishStreamTexture)
#endif
    IPC_MESSAGE_HANDLER(
        GpuChannelMsg_CollectRenderingStatsForSurface,
        OnCollectRenderingStatsForSurface)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  DCHECK(handled) << msg.type();
  return handled;
}

void GpuChannel::HandleMessage() {
  handle_messages_scheduled_ = false;

  if (!deferred_messages_.empty()) {
    IPC::Message* m = deferred_messages_.front();
    GpuCommandBufferStub* stub = stubs_.Lookup(m->routing_id());

    if (stub) {
      if (!stub->IsScheduled())
        return;
      if (stub->IsPreempted()) {
        OnScheduled();
        return;
      }
    }

    scoped_ptr<IPC::Message> message(m);
    deferred_messages_.pop_front();
    bool message_processed = true;

    processed_get_state_fast_ =
        (message->type() == GpuCommandBufferMsg_GetStateFast::ID);

    currently_processing_message_ = message.get();
    bool result;
    if (message->routing_id() == MSG_ROUTING_CONTROL)
      result = OnControlMessageReceived(*message);
    else
      result = router_.RouteMessage(*message);
    currently_processing_message_ = NULL;

    if (!result) {
      // Respond to sync messages even if router failed to route.
      if (message->is_sync()) {
        IPC::Message* reply = IPC::SyncMessage::GenerateReply(&*message);
        reply->set_reply_error();
        Send(reply);
      }
    } else {
      // If the command buffer becomes unscheduled as a result of handling the
      // message but still has more commands to process, synthesize an IPC
      // message to flush that command buffer.
      if (stub) {
        if (stub->HasUnprocessedCommands()) {
          deferred_messages_.push_front(new GpuCommandBufferMsg_Rescheduled(
              stub->route_id()));
          message_processed = false;
        }
      }
    }
    if (message_processed)
      MessageProcessed();
  }

  if (!deferred_messages_.empty()) {
    OnScheduled();
  }
}

void GpuChannel::OnCreateOffscreenCommandBuffer(
    const gfx::Size& size,
    const GPUCreateCommandBufferConfig& init_params,
    int32* route_id) {
  TRACE_EVENT0("gpu", "GpuChannel::OnCreateOffscreenCommandBuffer");
  GpuCommandBufferStub* share_group = stubs_.Lookup(init_params.share_group_id);

  *route_id = GenerateRouteID();

  scoped_ptr<GpuCommandBufferStub> stub(new GpuCommandBufferStub(
      this,
      share_group,
      gfx::GLSurfaceHandle(),
      mailbox_manager_.get(),
      image_manager_.get(),
      size,
      disallowed_features_,
      init_params.allowed_extensions,
      init_params.attribs,
      init_params.gpu_preference,
      *route_id,
      0, watchdog_,
      software_,
      init_params.active_url));
  if (preempted_flag_.get())
    stub->SetPreemptByFlag(preempted_flag_);
  router_.AddRoute(*route_id, stub.get());
  stubs_.AddWithID(stub.release(), *route_id);
  TRACE_EVENT1("gpu", "GpuChannel::OnCreateOffscreenCommandBuffer",
               "route_id", route_id);
}

void GpuChannel::OnDestroyCommandBuffer(int32 route_id) {
  TRACE_EVENT1("gpu", "GpuChannel::OnDestroyCommandBuffer",
               "route_id", route_id);

  if (router_.ResolveRoute(route_id)) {
    GpuCommandBufferStub* stub = stubs_.Lookup(route_id);
    bool need_reschedule = (stub && !stub->IsScheduled());
    router_.RemoveRoute(route_id);
    stubs_.Remove(route_id);
    // In case the renderer is currently blocked waiting for a sync reply from
    // the stub, we need to make sure to reschedule the GpuChannel here.
    if (need_reschedule)
      OnScheduled();
  }
}

#if defined(OS_ANDROID)
void GpuChannel::OnRegisterStreamTextureProxy(
    int32 stream_id,  const gfx::Size& initial_size, int32* route_id) {
  // Note that route_id is only used for notifications sent out from here.
  // StreamTextureManager owns all texture objects and for incoming messages
  // it finds the correct object based on stream_id.
  *route_id = GenerateRouteID();
  stream_texture_manager_->RegisterStreamTextureProxy(
      stream_id, initial_size, *route_id);
}

void GpuChannel::OnEstablishStreamTexture(
    int32 stream_id, SurfaceTexturePeer::SurfaceTextureTarget type,
    int32 primary_id, int32 secondary_id) {
  stream_texture_manager_->EstablishStreamTexture(
      stream_id, type, primary_id, secondary_id);
}
#endif

void GpuChannel::OnCollectRenderingStatsForSurface(
    int32 surface_id, GpuRenderingStats* stats) {
  for (StubMap::Iterator<GpuCommandBufferStub> it(&stubs_);
       !it.IsAtEnd(); it.Advance()) {
    int texture_upload_count =
        it.GetCurrentValue()->decoder()->GetTextureUploadCount();
    base::TimeDelta total_texture_upload_time =
        it.GetCurrentValue()->decoder()->GetTotalTextureUploadTime();
    base::TimeDelta total_processing_commands_time =
        it.GetCurrentValue()->decoder()->GetTotalProcessingCommandsTime();

    stats->global_texture_upload_count += texture_upload_count;
    stats->global_total_texture_upload_time += total_texture_upload_time;
    stats->global_total_processing_commands_time +=
        total_processing_commands_time;
    if (it.GetCurrentValue()->surface_id() == surface_id) {
      stats->texture_upload_count += texture_upload_count;
      stats->total_texture_upload_time += total_texture_upload_time;
      stats->total_processing_commands_time += total_processing_commands_time;
    }
  }
}

void GpuChannel::MessageProcessed() {
  messages_processed_++;
  if (preempting_flag_.get()) {
    io_message_loop_->PostTask(
        FROM_HERE,
        base::Bind(&SyncPointMessageFilter::MessageProcessed,
                   filter_, messages_processed_));
  }
}

}  // namespace content
