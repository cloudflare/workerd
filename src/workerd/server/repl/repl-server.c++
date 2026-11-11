// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-server.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/exception.h>
#include <workerd/server/factory/worker-factory.h>
#include <workerd/server/server/bridge.rs.h>

#include <kj-rs/kj-rs.h>

namespace workerd::server {

namespace {

kj::Exception loadFailed(kj::StringPtr path, kj::ArrayPtr<const kj::String> errors) {
  return JSG_KJ_EXCEPTION(
      FAILED, Error, "Failed to load ", path, ":\n  ", kj::strArray(errors, "\n  "));
}

::rust::Str toRust(kj::StringPtr str) {
  return ::rust::Str(str.begin(), str.size());
}

}  // namespace

kj::Promise<kj::Own<ReplConfigServer>> ReplConfigServer::start(const ReplServerResources& resources,
    const kj::HttpHeaderTable& replHeaderTable,
    kj::String path,
    kj::ArrayPtr<const capnp::word> config,
    bool experimental) {
  auto options = kj::heap<WorkerFactory::Options>();
  options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
  options->experimental = experimental;
  options->hostHeaderTable = replHeaderTable;
  auto words = config.asBytes();
  auto factory =
      kj::heap<WorkerFactory>(resources.v8System, resources.timer, resources.monotonicClock,
          resources.network, resources.entropySource, resources.fs, kj::mv(options),
          kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), config.size())
              .as<kj_rs::RustCopy>());
  // The server owns the factory for as long as it runs.
  auto self = kj::refcounted<ReplConfigServer>(
      kj::mv(path), new_in_process_server(kj::mv(factory), /*mock_internet=*/false));

  auto errors = co_await self->serve();
  if (errors.size() > 0) {
    // The config's errors are why the load failed, so a failure to close joins them rather than
    // taking their place.
    kj::Vector<kj::String> reported(errors.size() + 1);
    for (auto& error: errors) {
      reported.add(kj::mv(error));
    }
    try {
      co_await self->close();
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reported.add(kj::str("Closing the server then failed: ", exception.getDescription()));
    }
    kj::throwFatalException(loadFailed(self->path, reported));
  }
  co_return self;
}

ReplConfigServer::ReplConfigServer(kj::String path, ::rust::Box<InProcessServer> server)
    : path(kj::mv(path)),
      server(kj::mv(server)) {}

ReplConfigServer::~ReplConfigServer() noexcept(false) {
  // The REPL closes every server it loaded before it goes; a server dropped while running would
  // take its factory down without draining it.
  KJ_ASSERT(server == kj::none, "a REPL server was destroyed without being closed", path);
}

kj::Promise<kj::Array<kj::String>> ReplConfigServer::serve() {
  auto& running = *KJ_ASSERT_NONNULL(server);
  auto failure = co_await running.start().then([]() -> kj::Maybe<kj::Exception> {
    return kj::none;
  }, [](kj::Exception&& e) -> kj::Maybe<kj::Exception> { return kj::mv(e); });

  kj::Vector<kj::String> errors;
  kj::Vector<kj::String> serverWarnings;
  for (auto& report: running.take_reports()) {
    (report.error ? errors : serverWarnings).add(kj::str(report.message));
  }
  warnings = serverWarnings.releaseAsArray();
  KJ_IF_SOME(e, failure) {
    if (errors.empty()) {
      errors.add(kj::str(e.getDescription()));
    }
  }
  co_return errors.releaseAsArray();
}

InProcessServer& ReplConfigServer::requireOpen() {
  KJ_IF_SOME(s, server) {
    if (!isClosed()) return *s;
  }
  JSG_FAIL_REQUIRE(Error, "The server for ", path, " was closed.");
}

kj::Rc<IoChannelFactory::SubrequestChannel> ReplConfigServer::service(
    kj::StringPtr name, kj::Maybe<kj::StringPtr> entrypoint, kj::Maybe<Frankenvalue> props) {
  auto rustEntrypoint = entrypoint.map([](kj::StringPtr e) { return toRust(e); });
  auto ownProps = props.map([](Frankenvalue& p) { return kj::heap<Frankenvalue>(kj::mv(p)); });
  return subrequest_channel_into_kj(
      requireOpen().service(toRust(name), kj::mv(rustEntrypoint), kj::mv(ownProps)));
}

kj::Rc<IoChannelFactory::SubrequestChannel> ReplConfigServer::socket(kj::StringPtr name) {
  return subrequest_channel_into_kj(requireOpen().socket(toRust(name)));
}

kj::Promise<void> ReplConfigServer::close() {
  KJ_IF_SOME(c, closing) {
    return c.addBranch();
  }
  return closing.emplace(closeServer().fork()).addBranch();
}

kj::Promise<void> ReplConfigServer::closeServer() {
  // The server's isolates end on the event loop, not under the lock of the isolate that asked for
  // the close.
  co_await kj::yield();
  KJ_IF_SOME(s, server) {
    // What the REPL holds of the server goes first: anything that still holds a request or a
    // channel would keep the factory alive.
    auto reason = kj::str("The server for ", path, " was closed.");
    s->revoke(toRust(reason));
    auto closing = close_in_process_server(kj::mv(s));
    server = kj::none;
    co_await closing;
  }
}

}  // namespace workerd::server
