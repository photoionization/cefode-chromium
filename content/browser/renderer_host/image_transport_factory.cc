// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/image_transport_factory.h"

#include <algorithm>
#include <map>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/observer_list.h"
#include "base/threading/non_thread_safe.h"
#include "cc/output_surface.h"
#include "cc/output_surface_client.h"
#include "content/browser/gpu/browser_gpu_channel_host_factory.h"
#include "content/browser/gpu/gpu_data_manager_impl.h"
#include "content/browser/gpu/gpu_process_host.h"
#include "content/browser/gpu/gpu_surface_tracker.h"
#include "content/common/gpu/client/context_provider_command_buffer.h"
#include "content/common/gpu/client/gl_helper.h"
#include "content/common/gpu/client/gpu_channel_host.h"
#include "content/common/gpu/client/webgraphicscontext3d_command_buffer_impl.h"
#include "content/common/gpu/gpu_messages.h"
#include "content/common/gpu/gpu_process_launch_causes.h"
#include "content/common/webkitplatformsupport_impl.h"
#include "content/public/common/content_switches.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/ipc/command_buffer_proxy.h"
#include "third_party/khronos/GLES2/gl2.h"
#include "third_party/khronos/GLES2/gl2ext.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_setup.h"
#include "ui/compositor/test_web_graphics_context_3d.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/gfx/size.h"

#if defined(OS_WIN)
#include "ui/surface/accelerated_surface_win.h"
#endif

namespace content {
namespace {

ImageTransportFactory* g_factory;

class DefaultTransportFactory
    : public ui::DefaultContextFactory,
      public ImageTransportFactory {
 public:
  DefaultTransportFactory() {
    ui::DefaultContextFactory::Initialize();
  }

  virtual ui::ContextFactory* AsContextFactory() OVERRIDE {
    return this;
  }

  virtual gfx::GLSurfaceHandle CreateSharedSurfaceHandle() OVERRIDE {
    return gfx::GLSurfaceHandle();
  }

  virtual void DestroySharedSurfaceHandle(
      gfx::GLSurfaceHandle surface) OVERRIDE {
  }

  virtual scoped_refptr<ui::Texture> CreateTransportClient(
      float device_scale_factor) OVERRIDE {
    return NULL;
  }

  virtual scoped_refptr<ui::Texture> CreateOwnedTexture(
      const gfx::Size& size,
      float device_scale_factor,
      unsigned int texture_id) OVERRIDE {
    return NULL;
  }

  virtual GLHelper* GetGLHelper() OVERRIDE {
    return NULL;
  }

  virtual uint32 InsertSyncPoint() OVERRIDE {
    return 0;
  }

  // We don't generate lost context events, so we don't need to keep track of
  // observers
  virtual void AddObserver(ImageTransportFactoryObserver* observer) OVERRIDE {
  }

  virtual void RemoveObserver(
      ImageTransportFactoryObserver* observer) OVERRIDE {
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(DefaultTransportFactory);
};

class OwnedTexture : public ui::Texture, ImageTransportFactoryObserver {
 public:
  OwnedTexture(WebKit::WebGraphicsContext3D* host_context,
               const gfx::Size& size,
               float device_scale_factor,
               unsigned int texture_id)
      : ui::Texture(true, size, device_scale_factor),
        host_context_(host_context),
        texture_id_(texture_id) {
    ImageTransportFactory::GetInstance()->AddObserver(this);
  }

  // ui::Texture overrides:
  virtual unsigned int PrepareTexture() OVERRIDE {
    return texture_id_;
  }

  virtual WebKit::WebGraphicsContext3D* HostContext3D() OVERRIDE {
    return host_context_;
  }

  // ImageTransportFactory overrides:
  virtual void OnLostResources() OVERRIDE {
    DeleteTexture();
  }

 protected:
  virtual ~OwnedTexture() {
    ImageTransportFactory::GetInstance()->RemoveObserver(this);
    DeleteTexture();
  }

 protected:
  void DeleteTexture() {
    if (texture_id_) {
      host_context_->deleteTexture(texture_id_);
      texture_id_ = 0;
    }
  }

  // A raw pointer. This |ImageTransportClientTexture| will be destroyed
  // before the |host_context_| via
  // |ImageTransportFactoryObserver::OnLostContext()| handlers.
  WebKit::WebGraphicsContext3D* host_context_;
  unsigned texture_id_;

  DISALLOW_COPY_AND_ASSIGN(OwnedTexture);
};

class ImageTransportClientTexture : public OwnedTexture {
 public:
  ImageTransportClientTexture(
      WebKit::WebGraphicsContext3D* host_context,
      float device_scale_factor)
      : OwnedTexture(host_context,
                     gfx::Size(0, 0),
                     device_scale_factor,
                     host_context->createTexture()) {
  }

  virtual void Consume(const std::string& mailbox_name,
                       const gfx::Size& new_size) OVERRIDE {
    DCHECK(mailbox_name.size() == GL_MAILBOX_SIZE_CHROMIUM);
    mailbox_name_ = mailbox_name;
    if (mailbox_name.empty())
      return;

    DCHECK(host_context_ && texture_id_);
    host_context_->bindTexture(GL_TEXTURE_2D, texture_id_);
    host_context_->consumeTextureCHROMIUM(
        GL_TEXTURE_2D,
        reinterpret_cast<const signed char*>(mailbox_name.c_str()));
    size_ = new_size;
    host_context_->shallowFlushCHROMIUM();
  }

  virtual std::string Produce() OVERRIDE {
    std::string name;
    if (!mailbox_name_.empty()) {
      DCHECK(host_context_ && texture_id_);
      host_context_->bindTexture(GL_TEXTURE_2D, texture_id_);
      host_context_->produceTextureCHROMIUM(
          GL_TEXTURE_2D,
          reinterpret_cast<const signed char*>(mailbox_name_.c_str()));
      mailbox_name_.swap(name);
    }
    return name;
  }

 protected:
  virtual ~ImageTransportClientTexture() {}

 private:
  std::string mailbox_name_;
  DISALLOW_COPY_AND_ASSIGN(ImageTransportClientTexture);
};

class GpuProcessTransportFactory;

class CompositorSwapClient
    : public base::SupportsWeakPtr<CompositorSwapClient>,
      public WebGraphicsContext3DSwapBuffersClient {
 public:
  CompositorSwapClient(ui::Compositor* compositor,
                       GpuProcessTransportFactory* factory)
      : compositor_(compositor),
        factory_(factory) {
  }

  virtual ~CompositorSwapClient() {
  }

  virtual void OnViewContextSwapBuffersPosted() OVERRIDE {
    compositor_->OnSwapBuffersPosted();
  }

  virtual void OnViewContextSwapBuffersComplete() OVERRIDE {
    compositor_->OnSwapBuffersComplete();
  }

  virtual void OnViewContextSwapBuffersAborted() OVERRIDE {
    // Recreating contexts directly from here causes issues, so post a task
    // instead.
    // TODO(piman): Fix the underlying issues.
    MessageLoop::current()->PostTask(FROM_HERE,
        base::Bind(&CompositorSwapClient::OnLostContext, this->AsWeakPtr()));
  }

 private:
  void OnLostContext();
  ui::Compositor* compositor_;
  GpuProcessTransportFactory* factory_;

  DISALLOW_COPY_AND_ASSIGN(CompositorSwapClient);
};

class BrowserCompositorOutputSurface;

// Directs vsync updates to the appropriate BrowserCompositorOutputSurface.
class BrowserCompositorOutputSurfaceProxy
    : public base::RefCountedThreadSafe<BrowserCompositorOutputSurfaceProxy> {
 public:
  BrowserCompositorOutputSurfaceProxy()
    : message_handler_set_(false) {
  }

  void AddSurface(BrowserCompositorOutputSurface* surface, int surface_id) {
    if (!message_handler_set_) {
      uint32 messages_to_filter[] = {GpuHostMsg_UpdateVSyncParameters::ID};
      BrowserGpuChannelHostFactory::instance()->SetHandlerForControlMessages(
          messages_to_filter,
          arraysize(messages_to_filter),
          base::Bind(&BrowserCompositorOutputSurfaceProxy::OnMessageReceived,
                     this),
          MessageLoop::current()->message_loop_proxy());
      message_handler_set_ = true;
    }
    surface_map_.AddWithID(surface, surface_id);
  }

  void RemoveSurface(int surface_id) {
    surface_map_.Remove(surface_id);
  }

 private:
  void OnMessageReceived(const IPC::Message& message) {
    IPC_BEGIN_MESSAGE_MAP(BrowserCompositorOutputSurfaceProxy, message)
      IPC_MESSAGE_HANDLER(GpuHostMsg_UpdateVSyncParameters,
                          OnUpdateVSyncParameters);
    IPC_END_MESSAGE_MAP()
  }

  void OnUpdateVSyncParameters(int surface_id,
                               base::TimeTicks timebase,
                               base::TimeDelta interval);

  friend class
      base::RefCountedThreadSafe<BrowserCompositorOutputSurfaceProxy>;
  ~BrowserCompositorOutputSurfaceProxy() {}
  IDMap<BrowserCompositorOutputSurface> surface_map_;
  bool message_handler_set_;

  DISALLOW_COPY_AND_ASSIGN(BrowserCompositorOutputSurfaceProxy);
};

// Adapts a WebGraphicsContext3DCommandBufferImpl into a
// cc::OutputSurface that also handles vsync parameter updates
// arriving from the GPU process.
class BrowserCompositorOutputSurface
    : public cc::OutputSurface,
      public base::NonThreadSafe {
 public:
  BrowserCompositorOutputSurface(
      WebGraphicsContext3DCommandBufferImpl* context,
      int surface_id,
      BrowserCompositorOutputSurfaceProxy* output_surface_proxy,
      base::MessageLoopProxy* compositor_message_loop,
      base::WeakPtr<ui::Compositor> compositor)
      : OutputSurface(scoped_ptr<WebKit::WebGraphicsContext3D>(context)),
        surface_id_(surface_id),
        output_surface_proxy_(output_surface_proxy),
        compositor_message_loop_(compositor_message_loop),
        compositor_(compositor) {
    DetachFromThread();
  }

  virtual ~BrowserCompositorOutputSurface() {
    DCHECK(CalledOnValidThread());
    if (!client_)
      return;
    output_surface_proxy_->RemoveSurface(surface_id_);
  }

  virtual bool BindToClient(
      cc::OutputSurfaceClient* client) OVERRIDE {
    DCHECK(CalledOnValidThread());

    if (!OutputSurface::BindToClient(client))
      return false;

    output_surface_proxy_->AddSurface(this, surface_id_);
    return true;
  }

  void OnUpdateVSyncParameters(
      base::TimeTicks timebase, base::TimeDelta interval) {
    DCHECK(CalledOnValidThread());
    DCHECK(client_);
    client_->OnVSyncParametersChanged(timebase, interval);
    compositor_message_loop_->PostTask(
        FROM_HERE,
        base::Bind(&ui::Compositor::OnUpdateVSyncParameters,
                   compositor_, timebase, interval));
  }

 private:
  int surface_id_;
  scoped_refptr<BrowserCompositorOutputSurfaceProxy> output_surface_proxy_;

  scoped_refptr<base::MessageLoopProxy> compositor_message_loop_;
  base::WeakPtr<ui::Compositor> compositor_;
};

void BrowserCompositorOutputSurfaceProxy::OnUpdateVSyncParameters(
    int surface_id, base::TimeTicks timebase, base::TimeDelta interval) {
  BrowserCompositorOutputSurface* surface = surface_map_.Lookup(surface_id);
  if (surface)
    surface->OnUpdateVSyncParameters(timebase, interval);
}

class GpuProcessTransportFactory
    : public ui::ContextFactory,
      public ImageTransportFactory {
 public:
  GpuProcessTransportFactory()
      : ALLOW_THIS_IN_INITIALIZER_LIST(callback_factory_(this)) {
    output_surface_proxy_ = new BrowserCompositorOutputSurfaceProxy();
  }

  virtual ~GpuProcessTransportFactory() {
    DCHECK(per_compositor_data_.empty());
  }

  virtual WebGraphicsContext3DCommandBufferImpl* CreateOffscreenContext()
      OVERRIDE {
    base::WeakPtr<WebGraphicsContext3DSwapBuffersClient> swap_client;
    return CreateContextCommon(swap_client, 0);
  }

  virtual cc::OutputSurface* CreateOutputSurface(
      ui::Compositor* compositor) OVERRIDE {
    PerCompositorData* data = per_compositor_data_[compositor];
    if (!data)
      data = CreatePerCompositorData(compositor);
    WebGraphicsContext3DCommandBufferImpl* context =
        CreateContextCommon(data->swap_client->AsWeakPtr(),
                            data->surface_id);
    return new BrowserCompositorOutputSurface(
        context,
        per_compositor_data_[compositor]->surface_id,
        output_surface_proxy_,
        base::MessageLoopProxy::current(),
        compositor->AsWeakPtr());
  }

  virtual void RemoveCompositor(ui::Compositor* compositor) OVERRIDE {
    PerCompositorDataMap::iterator it = per_compositor_data_.find(compositor);
    if (it == per_compositor_data_.end())
      return;
    PerCompositorData* data = it->second;
    DCHECK(data);
    GpuSurfaceTracker::Get()->RemoveSurface(data->surface_id);
    delete data;
    per_compositor_data_.erase(it);
    if (per_compositor_data_.empty()) {
      gl_helper_.reset();
      callback_factory_.InvalidateWeakPtrs();
    }
  }

  virtual ui::ContextFactory* AsContextFactory() OVERRIDE {
    return this;
  }

  virtual gfx::GLSurfaceHandle CreateSharedSurfaceHandle() OVERRIDE {
    CreateSharedContextLazy();
    gfx::GLSurfaceHandle handle = gfx::GLSurfaceHandle(
        gfx::kNullPluginWindow, gfx::TEXTURE_TRANSPORT);
    handle.parent_gpu_process_id =
        shared_contexts_main_thread_->Context3d()->GetGPUProcessID();
    handle.parent_client_id =
        shared_contexts_main_thread_->Context3d()->GetChannelID();
    return handle;
  }

  virtual void DestroySharedSurfaceHandle(
      gfx::GLSurfaceHandle surface) OVERRIDE {
  }

  virtual scoped_refptr<ui::Texture> CreateTransportClient(
      float device_scale_factor) OVERRIDE {
    if (!shared_contexts_main_thread_)
        return NULL;
    scoped_refptr<ImageTransportClientTexture> image(
        new ImageTransportClientTexture(
            shared_contexts_main_thread_->Context3d(),
            device_scale_factor));
    return image;
  }

  virtual scoped_refptr<ui::Texture> CreateOwnedTexture(
      const gfx::Size& size,
      float device_scale_factor,
      unsigned int texture_id) OVERRIDE {
    if (!shared_contexts_main_thread_)
        return NULL;
    scoped_refptr<OwnedTexture> image(new OwnedTexture(
        shared_contexts_main_thread_->Context3d(),
        size,
        device_scale_factor,
        texture_id));
    return image;
  }

  virtual GLHelper* GetGLHelper() OVERRIDE {
    if (!gl_helper_.get()) {
      CreateSharedContextLazy();
      WebKit::WebGraphicsContext3D* context_for_main_thread =
          shared_contexts_main_thread_->Context3d();
      WebKit::WebGraphicsContext3D* context_for_thread =
          CreateOffscreenContext();
      if (!context_for_thread)
        return NULL;

      gl_helper_.reset(new GLHelper(context_for_main_thread,
                                    context_for_thread));
    }
    return gl_helper_.get();
  }

  virtual uint32 InsertSyncPoint() OVERRIDE {
    if (!shared_contexts_main_thread_)
      return 0;
    return shared_contexts_main_thread_->Context3d()->insertSyncPoint();
  }

  virtual void AddObserver(ImageTransportFactoryObserver* observer) OVERRIDE {
    observer_list_.AddObserver(observer);
  }

  virtual void RemoveObserver(
      ImageTransportFactoryObserver* observer) OVERRIDE {
    observer_list_.RemoveObserver(observer);
  }

  void OnLostContext(ui::Compositor* compositor) {
    LOG(ERROR) << "Lost UI compositor context.";
    PerCompositorData* data = per_compositor_data_[compositor];
    DCHECK(data);

    // Prevent callbacks from other contexts in the same share group from
    // calling us again.
    data->swap_client.reset(new CompositorSwapClient(compositor, this));
    compositor->OnSwapBuffersAborted();
  }

 private:
  struct PerCompositorData {
    int surface_id;
    scoped_ptr<CompositorSwapClient> swap_client;
#if defined(OS_WIN)
    scoped_ptr<AcceleratedSurface> accelerated_surface;
#endif
  };

  PerCompositorData* CreatePerCompositorData(ui::Compositor* compositor) {
    DCHECK(!per_compositor_data_[compositor]);

    CreateSharedContextLazy();

    gfx::AcceleratedWidget widget = compositor->widget();
    GpuSurfaceTracker* tracker = GpuSurfaceTracker::Get();

    PerCompositorData* data = new PerCompositorData;
    data->surface_id = tracker->AddSurfaceForNativeWidget(widget);
    data->swap_client.reset(new CompositorSwapClient(compositor, this));
#if defined(OS_WIN)
    if (GpuDataManagerImpl::GetInstance()->IsUsingAcceleratedSurface())
      data->accelerated_surface.reset(new AcceleratedSurface(widget));
#endif
    tracker->SetSurfaceHandle(
        data->surface_id,
        gfx::GLSurfaceHandle(widget, gfx::NATIVE_DIRECT));

    per_compositor_data_[compositor] = data;

    return data;
  }

  WebGraphicsContext3DCommandBufferImpl* CreateContextCommon(
      const base::WeakPtr<WebGraphicsContext3DSwapBuffersClient>& swap_client,
      int surface_id) {
    WebKit::WebGraphicsContext3D::Attributes attrs;
    attrs.shareResources = true;
    attrs.depth = false;
    attrs.stencil = false;
    attrs.antialias = false;
    attrs.noAutomaticFlushes = true;
    GpuChannelHostFactory* factory = BrowserGpuChannelHostFactory::instance();
    GURL url("chrome://gpu/GpuProcessTransportFactory::CreateContextCommon");
    scoped_ptr<WebGraphicsContext3DCommandBufferImpl> context(
        new WebGraphicsContext3DCommandBufferImpl(
            surface_id,
            url,
            factory,
            swap_client));
    if (!context->Initialize(
        attrs,
        false,
        CAUSE_FOR_GPU_LAUNCH_WEBGRAPHICSCONTEXT3DCOMMANDBUFFERIMPL_INITIALIZE))
      return NULL;
    return context.release();
  }

  class MainThreadContextProvider : public ContextProviderCommandBuffer {
   public:
    explicit MainThreadContextProvider(GpuProcessTransportFactory* factory)
        : factory_(factory) {}

   protected:
    virtual ~MainThreadContextProvider() {}

    virtual scoped_ptr<WebGraphicsContext3DCommandBufferImpl>
        CreateOffscreenContext3d() {
      return make_scoped_ptr(factory_->CreateOffscreenContext());
    }

    virtual void OnLostContext() OVERRIDE {
      ContextProviderCommandBuffer::OnLostContext();

      MessageLoop::current()->PostTask(
          FROM_HERE,
          base::Bind(&GpuProcessTransportFactory::OnLostMainThreadSharedContext,
                     factory_->callback_factory_.GetWeakPtr()));
    }

   private:
    GpuProcessTransportFactory* factory_;
  };

  virtual scoped_refptr<cc::ContextProvider>
      OffscreenContextProviderForMainThread() OVERRIDE {
    if (!shared_contexts_main_thread_ ||
        shared_contexts_main_thread_->DestroyedOnMainThread()) {
      shared_contexts_main_thread_ = new MainThreadContextProvider(this);
    }
    return shared_contexts_main_thread_;
  }

  class CompositorThreadContextProvider : public ContextProviderCommandBuffer {
   public:
    explicit CompositorThreadContextProvider(
        GpuProcessTransportFactory* factory) : factory_(factory) {}

   protected:
    virtual ~CompositorThreadContextProvider() {}

    virtual scoped_ptr<WebGraphicsContext3DCommandBufferImpl>
        CreateOffscreenContext3d() {
      return make_scoped_ptr(factory_->CreateOffscreenContext());
    }

   private:
    GpuProcessTransportFactory* factory_;
  };

  virtual scoped_refptr<cc::ContextProvider>
      OffscreenContextProviderForCompositorThread() OVERRIDE {
    if (!shared_contexts_compositor_thread_ ||
        shared_contexts_compositor_thread_->DestroyedOnMainThread()) {
      shared_contexts_compositor_thread_ =
          new CompositorThreadContextProvider(this);
    }
    return shared_contexts_compositor_thread_;
  }

  void CreateSharedContextLazy() {
    scoped_refptr<cc::ContextProvider> provider =
        OffscreenContextProviderForMainThread();
    if (!provider->InitializeOnMainThread()) {
      // If we can't recreate contexts, we won't be able to show the UI.
      // Better crash at this point.
      LOG(FATAL) << "Failed to initialize UI shared context.";
    }
    if (!provider->BindToCurrentThread()) {
      // If we can't recreate contexts, we won't be able to show the UI.
      // Better crash at this point.
      LOG(FATAL) << "Failed to make UI shared context current.";
    }
  }

  void OnLostMainThreadSharedContext() {
    // Keep old resources around while we call the observers, but ensure that
    // new resources are created if needed.

    scoped_refptr<MainThreadContextProvider> old_contexts_main_thread =
        shared_contexts_main_thread_;
    shared_contexts_main_thread_ = NULL;

    scoped_ptr<GLHelper> old_helper(gl_helper_.release());

    FOR_EACH_OBSERVER(ImageTransportFactoryObserver,
        observer_list_,
        OnLostResources());
  }

  typedef std::map<ui::Compositor*, PerCompositorData*> PerCompositorDataMap;
  PerCompositorDataMap per_compositor_data_;
  scoped_refptr<MainThreadContextProvider> shared_contexts_main_thread_;
  scoped_refptr<CompositorThreadContextProvider>
      shared_contexts_compositor_thread_;
  scoped_ptr<GLHelper> gl_helper_;
  ObserverList<ImageTransportFactoryObserver> observer_list_;
  base::WeakPtrFactory<GpuProcessTransportFactory> callback_factory_;
  scoped_refptr<BrowserCompositorOutputSurfaceProxy> output_surface_proxy_;

  DISALLOW_COPY_AND_ASSIGN(GpuProcessTransportFactory);
};

void CompositorSwapClient::OnLostContext() {
  factory_->OnLostContext(compositor_);
  // Note: previous line destroyed this. Don't access members from now on.
}

WebKit::WebGraphicsContext3D* CreateTestContext() {
  ui::TestWebGraphicsContext3D* test_context =
      new ui::TestWebGraphicsContext3D();
  test_context->Initialize();
  return test_context;
}

}  // anonymous namespace

// static
void ImageTransportFactory::Initialize() {
  CommandLine* command_line = CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kTestCompositor)) {
    ui::SetupTestCompositor();
  }
  if (ui::IsTestCompositorEnabled()) {
    g_factory = new DefaultTransportFactory();
    WebKitPlatformSupportImpl::SetOffscreenContextFactoryForTest(
        CreateTestContext);
  } else {
    g_factory = new GpuProcessTransportFactory();
  }
  ui::ContextFactory::SetInstance(g_factory->AsContextFactory());
}

// static
void ImageTransportFactory::Terminate() {
  ui::ContextFactory::SetInstance(NULL);
  delete g_factory;
  g_factory = NULL;
}

// static
ImageTransportFactory* ImageTransportFactory::GetInstance() {
  return g_factory;
}

}  // namespace content
