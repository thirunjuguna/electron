// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/renderer/atom_renderer_client.h"

#include <string>
#include <vector>

#include "atom_natives.h"  // NOLINT: This file is generated with js2c

#include "atom/common/api/atom_bindings.h"
#include "atom/common/api/event_emitter_caller.h"
#include "atom/common/asar/asar_util.h"
#include "atom/common/atom_constants.h"
#include "atom/common/node_bindings.h"
#include "atom/common/options_switches.h"
#include "atom/renderer/api/atom_api_renderer_ipc.h"
#include "atom/renderer/atom_render_view_observer.h"
#include "atom/renderer/node_array_buffer_bridge.h"
#include "atom/renderer/web_worker_observer.h"
#include "base/command_line.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "native_mate/dictionary.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebScriptSource.h"

#include "atom/common/node_includes.h"

namespace atom {

namespace {

enum World {
  MAIN_WORLD = 0,
  // Use a high number far away from 0 to not collide with any other world
  // IDs created internally by Chrome.
  ISOLATED_WORLD = 999
};

enum ExtensionGroup {
  MAIN_GROUP = 1
};

// Helper class to forward the messages to the client.
class AtomRenderFrameObserver : public content::RenderFrameObserver {
 public:
  AtomRenderFrameObserver(content::RenderFrame* frame,
                          AtomRendererClient* renderer_client)
      : content::RenderFrameObserver(frame),
        render_frame_(frame),
        renderer_client_(renderer_client) {}

  // content::RenderFrameObserver:
  void DidClearWindowObject() override {
    renderer_client_->DidClearWindowObject(render_frame_);
  }

  void CreateIsolatedWorldContext() {
    auto frame = render_frame_->GetWebFrame();

    // This maps to the name shown in the context combo box in the Console tab
    // of the dev tools.
    frame->setIsolatedWorldHumanReadableName(
        World::ISOLATED_WORLD,
        blink::WebString::fromUTF8("Electron Isolated Context"));

    // Setup document's origin policy in isolated world
    frame->setIsolatedWorldSecurityOrigin(
      World::ISOLATED_WORLD, frame->document().getSecurityOrigin());

    // Create initial script context in isolated world
    blink::WebScriptSource source("void 0");
    frame->executeScriptInIsolatedWorld(
        World::ISOLATED_WORLD, &source, 1, ExtensionGroup::MAIN_GROUP);
  }

  void SetupMainWorldOverrides(v8::Handle<v8::Context> context) {
    // Setup window overrides in the main world context
    v8::Isolate* isolate = context->GetIsolate();

    // Wrap the bundle into a function that receives the binding object as
    // an argument.
    std::string bundle(node::isolated_bundle_data,
        node::isolated_bundle_data + sizeof(node::isolated_bundle_data));
    std::string wrapper = "(function (binding, require) {\n" + bundle + "\n})";
    auto script = v8::Script::Compile(
        mate::ConvertToV8(isolate, wrapper)->ToString());
    auto func = v8::Handle<v8::Function>::Cast(
        script->Run(context).ToLocalChecked());

    auto binding = v8::Object::New(isolate);
    api::Initialize(binding, v8::Null(isolate), context, nullptr);

    // Pass in CLI flags needed to setup window
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    mate::Dictionary dict(isolate, binding);
    if (command_line->HasSwitch(switches::kGuestInstanceID))
      dict.Set(options::kGuestInstanceID,
               command_line->GetSwitchValueASCII(switches::kGuestInstanceID));
    if (command_line->HasSwitch(switches::kOpenerID))
      dict.Set(options::kOpenerID,
               command_line->GetSwitchValueASCII(switches::kOpenerID));
    dict.Set("hiddenPage", command_line->HasSwitch(switches::kHiddenPage));

    v8::Local<v8::Value> args[] = { binding };
    ignore_result(func->Call(context, v8::Null(isolate), 1, args));
  }

  bool IsMainWorld(int world_id) {
    return world_id == World::MAIN_WORLD;
  }

  bool IsIsolatedWorld(int world_id) {
    return world_id == World::ISOLATED_WORLD;
  }

  bool ShouldNotifyClient(int world_id) {
    if (renderer_client_->isolated_world() && render_frame_->IsMainFrame())
      return IsIsolatedWorld(world_id);
    else
      return IsMainWorld(world_id);
  }

  void DidCreateScriptContext(v8::Handle<v8::Context> context,
                              int extension_group,
                              int world_id) override {
    if (ShouldNotifyClient(world_id))
      renderer_client_->DidCreateScriptContext(context, render_frame_);

    if (renderer_client_->isolated_world() && IsMainWorld(world_id)
        && render_frame_->IsMainFrame()) {
      CreateIsolatedWorldContext();
      SetupMainWorldOverrides(context);
    }
  }

  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int world_id) override {
    if (ShouldNotifyClient(world_id))
      renderer_client_->WillReleaseScriptContext(context, render_frame_);
  }

  void OnDestruct() override {
    delete this;
  }

 private:
  content::RenderFrame* render_frame_;
  AtomRendererClient* renderer_client_;

  DISALLOW_COPY_AND_ASSIGN(AtomRenderFrameObserver);
};

bool IsDevToolsExtension(content::RenderFrame* render_frame) {
  return static_cast<GURL>(render_frame->GetWebFrame()->document().url())
      .SchemeIs("chrome-extension");
}

}  // namespace

AtomRendererClient::AtomRendererClient()
    : node_integration_initialized_(false),
      node_bindings_(NodeBindings::Create(NodeBindings::RENDERER)),
      atom_bindings_(new AtomBindings(uv_default_loop())) {
  isolated_world_ = base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kContextIsolation);
}

AtomRendererClient::~AtomRendererClient() {
  asar::ClearArchives();
}

void AtomRendererClient::RenderThreadStarted() {
  OverrideNodeArrayBuffer();
  RendererClientBase::RenderThreadStarted();
}

void AtomRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  new AtomRenderFrameObserver(render_frame, this);
  RendererClientBase::RenderFrameCreated(render_frame);
}

void AtomRendererClient::RenderViewCreated(content::RenderView* render_view) {
  new AtomRenderViewObserver(render_view, this);
  RendererClientBase::RenderViewCreated(render_view);
}

void AtomRendererClient::DidClearWindowObject(
    content::RenderFrame* render_frame) {
  // Make sure every page will get a script context created.
  render_frame->GetWebFrame()->executeScript(blink::WebScriptSource("void 0"));
}

void AtomRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  // Inform the document start pharse.
  node::Environment* env = node_bindings_->uv_env();
  if (env) {
    v8::HandleScope handle_scope(env->isolate());
    mate::EmitEvent(env->isolate(), env->process_object(), "document-start");
  }
}

void AtomRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
  // Inform the document end pharse.
  node::Environment* env = node_bindings_->uv_env();
  if (env) {
    v8::HandleScope handle_scope(env->isolate());
    mate::EmitEvent(env->isolate(), env->process_object(), "document-end");
  }
}

void AtomRendererClient::DidCreateScriptContext(
    v8::Handle<v8::Context> context, content::RenderFrame* render_frame) {
  // Only allow node integration for the main frame, unless it is a devtools
  // extension page.
  if (!render_frame->IsMainFrame() && !IsDevToolsExtension(render_frame))
    return;

  // Prepare the node bindings.
  if (!node_integration_initialized_) {
    node_integration_initialized_ = true;
    node_bindings_->Initialize();
    node_bindings_->PrepareMessageLoop();
  }

  // Setup node environment for each window.
  node::Environment* env = node_bindings_->CreateEnvironment(context);

  // Add Electron extended APIs.
  atom_bindings_->BindTo(env->isolate(), env->process_object());
  AddRenderBindings(env->isolate(), env->process_object());

  // Load everything.
  node_bindings_->LoadEnvironment(env);

  if (node_bindings_->uv_env() == nullptr) {
    // Make uv loop being wrapped by window context.
    node_bindings_->set_uv_env(env);

    // Give the node loop a run to make sure everything is ready.
    node_bindings_->RunMessageLoop();
  }
}

void AtomRendererClient::WillReleaseScriptContext(
    v8::Handle<v8::Context> context, content::RenderFrame* render_frame) {
  // Only allow node integration for the main frame, unless it is a devtools
  // extension page.
  if (!render_frame->IsMainFrame() && !IsDevToolsExtension(render_frame))
    return;

  node::Environment* env = node::Environment::GetCurrent(context);
  if (env)
    mate::EmitEvent(env->isolate(), env->process_object(), "exit");

  // The main frame may be replaced.
  if (env == node_bindings_->uv_env())
    node_bindings_->set_uv_env(nullptr);

  // Destroy the node environment.
  node::FreeEnvironment(env);
  atom_bindings_->EnvironmentDestroyed(env);
}

bool AtomRendererClient::ShouldFork(blink::WebLocalFrame* frame,
                                    const GURL& url,
                                    const std::string& http_method,
                                    bool is_initial_navigation,
                                    bool is_server_redirect,
                                    bool* send_referrer) {
  // Handle all the navigations and reloads in browser.
  // FIXME We only support GET here because http method will be ignored when
  // the OpenURLFromTab is triggered, which means form posting would not work,
  // we should solve this by patching Chromium in future.
  *send_referrer = true;
  return http_method == "GET";
}

void AtomRendererClient::DidInitializeWorkerContextOnWorkerThread(
    v8::Local<v8::Context> context) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNodeIntegrationInWorker)) {
    WebWorkerObserver::GetCurrent()->ContextCreated(context);
  }
}

void AtomRendererClient::WillDestroyWorkerContextOnWorkerThread(
    v8::Local<v8::Context> context) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNodeIntegrationInWorker)) {
    WebWorkerObserver::GetCurrent()->ContextWillDestroy(context);
  }
}

v8::Local<v8::Context> AtomRendererClient::GetContext(
    blink::WebFrame* frame, v8::Isolate* isolate) {
  if (isolated_world())
    return frame->worldScriptContext(
        isolate, World::ISOLATED_WORLD, ExtensionGroup::MAIN_GROUP);
  else
    return frame->mainWorldScriptContext();
}

}  // namespace atom
