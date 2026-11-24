// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/api/actor.h>
#include <workerd/api/http.h>
#include <workerd/api/worker-loader-interop/ffi.rs.h>
#include <workerd/io/io-context.h>
#include <workerd/io/io-context/current-with-id.h>

#include <capnp/message.h>

namespace workerd::api::worker_loader_interop {

namespace {

// A view of text that Rust appended a NUL to.
kj::StringPtr textOf(const ::rust::String& text) {
  KJ_ASSERT(text.size() > 0 && text.data()[text.size() - 1] == '\0');
  return kj::StringPtr(text.data(), text.size() - 1);
}

kj::ArrayPtr<const byte> bytesOf(const ::rust::Vec<uint8_t>& bytes) {
  return kj::ArrayPtr<const byte>(bytes.data(), bytes.size());
}

kj::Maybe<kj::String> nameOf(bool hasName, ::rust::Str name) {
  if (!hasName) return kj::none;
  return kj::str(kj::ArrayPtr<const char>(name.data(), name.size()));
}

kj::Maybe<ResourceLimits> limitsOf(const Limits& limits) {
  if (!limits.present) return kj::none;
  ResourceLimits result;
  if (limits.has_cpu_ms) result.cpuMs = limits.cpu_ms;
  if (limits.has_sub_requests) result.subRequests = limits.sub_requests;
  return kj::mv(result);
}

}  // namespace

SourceBuilder::SourceBuilder(::rust::String mainModule)
    : content(kj::atomicRefcounted<SourceContent>()) {
  content->mainModule = kj::mv(mainModule);
}

kj::StringPtr SourceBuilder::keepText(::rust::String text) {
  // Each buffer is boxed, so the views stay valid as the vector grows.
  return textOf(*content->texts.add(kj::heap(kj::mv(text))));
}

kj::ArrayPtr<const byte> SourceBuilder::keepBytes(::rust::Vec<uint8_t> bytes) {
  return bytesOf(*content->bytes.add(kj::heap(kj::mv(bytes))));
}

void SourceBuilder::addText(uint8_t kind, ::rust::String name, ::rust::String body) {
  auto moduleName = keepText(kj::mv(name));
  auto text = keepText(kj::mv(body));
  WorkerSource::ModuleContent moduleContent = [&]() -> WorkerSource::ModuleContent {
    switch (static_cast<ModuleKind>(kind)) {
      case ModuleKind::Es:
        return WorkerSource::EsModule{.body = text.asArray()};
      case ModuleKind::CommonJs:
        return WorkerSource::CommonJsModule{.body = text};
      case ModuleKind::Text:
        return WorkerSource::TextModule{.body = text};
      case ModuleKind::Json:
        return WorkerSource::JsonModule{.body = text};
      case ModuleKind::Python:
        return WorkerSource::PythonModule{.body = text};
      case ModuleKind::Data:
      case ModuleKind::Wasm:
        break;
    }
    KJ_FAIL_ASSERT("not a text module kind", kind);
  }();
  modules.add(WorkerSource::Module{.name = moduleName, .content = kj::mv(moduleContent)});
}

void SourceBuilder::addBytes(uint8_t kind, ::rust::String name, ::rust::Vec<uint8_t> body) {
  auto moduleName = keepText(kj::mv(name));
  auto bytes = keepBytes(kj::mv(body));
  WorkerSource::ModuleContent moduleContent = [&]() -> WorkerSource::ModuleContent {
    switch (static_cast<ModuleKind>(kind)) {
      case ModuleKind::Data:
        return WorkerSource::DataModule{.body = bytes};
      case ModuleKind::Wasm:
        return WorkerSource::WasmModule{.body = bytes};
      default:
        break;
    }
    KJ_FAIL_ASSERT("not a byte module kind", kind);
  }();
  modules.add(WorkerSource::Module{.name = moduleName, .content = kj::mv(moduleContent)});
}

void SourceBuilder::addCompiledWasm(
    ::rust::String name, kj::Own<CompiledWasmModule> module, bool esmBuiltins, bool sourceUrl) {
  auto moduleName = keepText(kj::mv(name));
  // The wire bytes are owned by the compiled module, which the module content keeps.
  auto wireBytes = module->GetWireBytesRef();
  modules.add(WorkerSource::Module{.name = moduleName,
    .content = WorkerSource::WasmModule{
      .body = kj::arrayPtr(wireBytes.data(), wireBytes.size()),
      .compiledModule = *module,
      .compiledWithEsmIntegrationBuiltins = esmBuiltins,
      .compiledWithWasmSourceUrlSupport = sourceUrl,
    }});
}

void SourceBuilder::compileCompatFlags(jsg::Lock& js,
    kj::StringPtr compatDate,
    kj::ArrayPtr<const kj::String> flags,
    bool allowExperimental,
    CompatibilityDateValidation dateValidation) {
  capnp::word scratch[capnp::sizeInWords<CompatibilityFlags>() + 4]{};
  capnp::MallocMessageBuilder message(scratch);
  auto builder = message.getRoot<CompatibilityFlags>();
  SimpleWorkerErrorReporter errorReporter;
  // allowedExperimentalFlags is null on purpose, as in worker-loader.c++: a Worker Loader trusted
  // with specific experimental flags doesn't pass that trust on to its dynamic Workers.
  compileCompatibilityFlags(
      compatDate, flags, builder, errorReporter, allowExperimental, dateValidation, nullptr);
  if (!errorReporter.errors.empty()) {
    JSG_FAIL_REQUIRE(Error, errorReporter.errors.front());
  }
  content->compatFlags = capnp::clone(builder.asReader());
}

void SourceBuilder::setLimits(const Limits& limits) {
  this->limits = limitsOf(limits);
}

void SourceBuilder::setEnv(kj::Own<Frankenvalue> env) {
  this->env = kj::mv(*env);
}

void SourceBuilder::setGlobalOutbound(kj::Rc<SubrequestChannel> channel) {
  globalOutbound = kj::mv(channel);
}

void SourceBuilder::addTail(kj::Rc<SubrequestChannel> channel) {
  tails.add(kj::mv(channel));
}

void SourceBuilder::addStreamingTail(kj::Rc<SubrequestChannel> channel) {
  streamingTails.add(kj::mv(channel));
}

DynamicWorkerSource SourceBuilder::build() {
  KJ_ASSERT(content->compatFlags.get() != nullptr, "compat flags must be set");
  auto mainModule = textOf(content->mainModule);
  CompatibilityFlags::Reader compatFlags = *content->compatFlags;
  return {
    .source = WorkerSource(WorkerSource::ModulesSource{
      .mainModule = mainModule,
      .modules = modules.releaseAsArray(),
      .isPython = mainModule.endsWith(".py"_kj),
    }),
    .compatibilityFlags = compatFlags,
    .limits = kj::mv(limits),
    .env = kj::mv(env),
    .globalOutbound = kj::mv(globalOutbound),
    .tails = tails.releaseAsArray(),
    .streamingTails = streamingTails.releaseAsArray(),
    .ownContent = addRefContent(),
    .ownContentIsRpcResponse = false,
  };
}

kj::Own<SourceBuilder> source_builder_new(::rust::String mainModule) {
  return kj::heap<SourceBuilder>(kj::mv(mainModule));
}

void source_builder_add_text(
    SourceBuilder& builder, uint8_t kind, ::rust::String name, ::rust::String body) {
  builder.addText(kind, kj::mv(name), kj::mv(body));
}

void source_builder_add_bytes(
    SourceBuilder& builder, uint8_t kind, ::rust::String name, ::rust::Vec<uint8_t> body) {
  builder.addBytes(kind, kj::mv(name), kj::mv(body));
}

bool source_builder_compile_compat_flags(v8::Isolate* isolate,
    SourceBuilder& builder,
    ::rust::Str compatDate,
    ::rust::Slice<const ::rust::String> flags,
    bool allowExperimental,
    CompatibilityDateValidation dateValidation) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto date = kj::str(kj::ArrayPtr<const char>(compatDate.data(), compatDate.size()));
    auto ownFlags = KJ_MAP(flag, flags) {
      return kj::str(kj::ArrayPtr<const char>(flag.data(), flag.size()));
    };
    builder.compileCompatFlags(js, date, ownFlags, allowExperimental, dateValidation);
  });
}

void source_builder_set_limits(SourceBuilder& builder, const Limits& limits) {
  builder.setLimits(limits);
}

void source_builder_set_env(SourceBuilder& builder, kj::Own<Frankenvalue> env) {
  builder.setEnv(kj::mv(env));
}

void source_builder_set_global_outbound(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel) {
  builder.setGlobalOutbound(kj::mv(channel));
}

void source_builder_add_tail(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel) {
  builder.addTail(kj::mv(channel));
}

void source_builder_add_streaming_tail(SourceBuilder& builder, kj::Rc<SubrequestChannel> channel) {
  builder.addStreamingTail(kj::mv(channel));
}

bool source_builder_add_compiled_wasm(v8::Isolate* isolate,
    SourceBuilder& builder,
    ::rust::String name,
    kj::Own<CompiledWasmModule> module) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    builder.addCompiledWasm(kj::mv(name), kj::mv(module), js.isWasmEsmIntegrationBuiltinsEnabled(),
        js.isWasmSourceUrlSupportEnabled());
  });
}

bool source_builder_inherit_global_outbound(
    v8::Isolate* isolate, SourceBuilder& builder, uint64_t ioContextId) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& ioctx = io_context::currentWithId(ioContextId);
    // Transferability needn't be checked: what may be the parent's global outbound may be the
    // child's.
    builder.setGlobalOutbound(
        ioctx.getIoChannelFactory().getSubrequestChannel(IoContext::NULL_CLIENT_CHANNEL));
  });
}

bool worker_loader_load(v8::Isolate* isolate,
    uint64_t ioContextId,
    uint32_t channel,
    kj::Own<SourceBuilder> builder,
    kj::Maybe<kj::Own<WorkerStubHandle>>& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& ioctx = io_context::currentWithId(ioContextId);
    auto content = builder->addRefContent();
    auto source = builder->build();
    // The callback may be called any number of times, even without an ID: the runtime can evict
    // the isolate while a stub exists and no request is active on it, then recreate it on the
    // next request. It may destroy the content on another thread, which SourceContent allows.
    auto isolateChannel = ioctx.getIoChannelFactory().loadIsolate(
        channel, kj::none, [source = kj::mv(source), content = kj::mv(content)]() mutable {
      return source.clone(kj::atomicAddRef(*content));
    });
    result = kj::heap<WorkerStubHandle>(ioctx.addObject(kj::mv(isolateChannel)));
  });
}

bool worker_loader_get(v8::Isolate* isolate,
    uint64_t ioContextId,
    uint32_t channel,
    bool hasName,
    ::rust::Str name,
    ::rust::Box<CodeCallback> callback,
    kj::Maybe<kj::Own<WorkerStubHandle>>& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock&) {
    auto& ioctx = io_context::currentWithId(ioContextId);
    auto isolateChannel = ioctx.getIoChannelFactory().loadIsolate(channel, nameOf(hasName, name),
        [callback = kj::mv(callback)]() mutable -> kj::Promise<DynamicWorkerSource> {
      return code_callback_call(*callback).then(
          [](kj::Own<SourceBuilder> builder) { return builder->build(); });
    });
    result = kj::heap<WorkerStubHandle>(ioctx.addObject(kj::mv(isolateChannel)));
  });
}

bool worker_stub_get_entrypoint(v8::Isolate* isolate,
    const WorkerStubHandle& stub,
    uint64_t ioContextId,
    bool hasName,
    ::rust::Str name,
    kj::Own<Frankenvalue> props,
    const Limits& limits,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<Fetcher>(isolate, result, [&](jsg::Lock& js) -> jsg::Ref<Fetcher> {
    auto& ioctx = io_context::currentWithId(ioContextId);
    auto& channel = *const_cast<WorkerStubHandle&>(stub).channel;
    auto subreqChannel =
        channel.getEntrypoint(nameOf(hasName, name), kj::mv(*props), limitsOf(limits));
    return js.alloc<Fetcher>(ioctx.addObject(kj::mv(subreqChannel)));
  });
}

bool worker_stub_get_actor_class(v8::Isolate* isolate,
    const WorkerStubHandle& stub,
    uint64_t ioContextId,
    bool hasName,
    ::rust::Str name,
    kj::Own<Frankenvalue> props,
    const Limits& limits,
    kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<DurableObjectClass>(
      isolate, result, [&](jsg::Lock& js) -> jsg::Ref<DurableObjectClass> {
    auto& ioctx = io_context::currentWithId(ioContextId);
    auto& channel = *const_cast<WorkerStubHandle&>(stub).channel;
    return js.alloc<DurableObjectClass>(ioctx.addObject(
        channel.getActorClass(nameOf(hasName, name), kj::mv(*props), limitsOf(limits))));
  });
}

}  // namespace workerd::api::worker_loader_interop
