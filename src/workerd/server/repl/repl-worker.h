// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/io/io-thread-context.h>
#include <workerd/io/worker.h>
#include <workerd/server/repl/repl.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/http-over-capnp.h>
#include <capnp/message.h>

namespace workerd::api {
class MemoryCacheProvider;
}  // namespace workerd::api

namespace workerd::server::repl {

// A worker that `workerd repl` evaluates input in (cli/repl/environment.rs), and that never
// receives an event. Rust opens one request on it for the whole session, whose IoContext every
// input runs in.
//
// The worker exists because the APIs need one: an IoContext belongs to a worker, and the global
// scope is a worker's. Its main module is empty and it has no bindings; Rust gives its global
// scope `workerd`, through which the user loads configs. It always uses the new module registry,
// whose built-in `node-internal:internal_inspect` renders results (repl_inspect()), and under
// which a dynamic `import()` from the REPL resolves as it would from the worker's main module.
//
// Lives on the thread of the event loop it was created on, and must be used only from there.
class ReplWorker::Impl {
 public:
  // Throws, naming the problems, if the compatibility settings are not valid.
  Impl(const ReplServerResources& resources, const ReplWorkerOptions& options);
  ~Impl() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Impl);

  // A new request on the worker, not yet delivered, whose IoContext's channels reach only the
  // clock: the REPL's JavaScript has no bindings, and reaches the loaded configs through the
  // channels their servers hand out.
  kj::Own<IoContext::IncomingRequest> newRequest();

  // The name input is compiled under: the URL of the worker's main module. A dynamic import's
  // referrer determines where it resolves, so an `import()` typed into the REPL resolves as one in
  // the main module would.
  kj::StringPtr getInputName() const {
    return inputName;
  }

  // What the servers of the configs loaded in this worker's environment are built from.
  kj::Rc<ReplFactorySource> getFactorySource();

 private:
  class ChannelFactory;

  struct ErrorReporter final: public Worker::ValidationErrorReporter {
    kj::Vector<kj::String> errors;
    kj::Vector<kj::String> warnings;

    void addError(kj::String error) override {
      errors.add(kj::mv(error));
    }
    void addWarning(kj::String warning) override {
      warnings.add(kj::mv(warning));
    }
    void addEntrypoint(
        kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {}
    void addActorClass(kj::StringPtr exportName) override {}
    void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {}

    // Throws, listing the errors, if there are any.
    void check(kj::StringPtr what);
  };

  ReplServerResources resources;

  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  ThreadContext::HeaderIdBundle threadContextHeaderBundle;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<TimerChannel> timerChannel;

  // Owns the compatibility flags `featureFlags` reads.
  capnp::MallocMessageBuilder featureFlagsMessage;
  CompatibilityFlags::Reader featureFlags;

  kj::Own<api::MemoryCacheProvider> memoryCacheProvider;

  kj::String inputName;

  kj::Own<Worker::Isolate> isolate;
  kj::Own<const Worker::Script> script;
  kj::Own<const Worker> worker;

  kj::Own<kj::HttpHeaderTable> headerTable;
  // None only while the worker is constructed.
  kj::Maybe<kj::Rc<ReplFactorySource>> factorySource;

  void compileFlags(const ReplWorkerOptions& options);
  void compileWorker();
};

}  // namespace workerd::server::repl
