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

kj::Maybe<::rust::Str> toRust(kj::Maybe<kj::StringPtr> str) {
  return str.map([](kj::StringPtr s) { return toRust(s); });
}

// What a started server reported: the config's errors, and its warnings.
struct Reports {
  kj::Array<kj::String> errors;
  kj::Array<kj::String> warnings;
};

// Starts `running`'s run; the config errors, or the failure to start if it reported none.
kj::Promise<Reports> serve(InProcessServer& running) {
  auto failure = co_await running.start().then([]() -> kj::Maybe<kj::Exception> {
    return kj::none;
  }, [](kj::Exception&& e) -> kj::Maybe<kj::Exception> { return kj::mv(e); });

  kj::Vector<kj::String> errors;
  kj::Vector<kj::String> warnings;
  for (auto& report: running.take_reports()) {
    (report.error ? errors : warnings).add(kj::str(report.message));
  }
  KJ_IF_SOME(e, failure) {
    if (errors.empty()) {
      errors.add(kj::str(e.getDescription()));
    }
  }
  co_return Reports{.errors = errors.releaseAsArray(), .warnings = warnings.releaseAsArray()};
}

struct Started {
  ::rust::Box<InProcessServer> server;
  kj::Array<kj::String> warnings;
};

// Loads the config `source` names, with `watch` watching the files the load reads, and starts a
// server on it. Throws a JSG error listing the problems if it doesn't start, having closed it.
kj::Promise<Started> start(const ReplServerContext& context,
    const ReplConfigSource& source,
    kj::Maybe<ReplConfigWatch&> watch) {
  auto config = context.loader.load(source.path,
      source.constName.map([](const kj::String& name) { return name.asPtr(); }),
      context.experimental, watch);
  auto options = kj::heap<WorkerFactory::Options>();
  options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
  options->experimental = context.experimental;
  options->hostHeaderTable = context.replHeaderTable;
  auto words = config.asBytes();
  auto& resources = context.resources;
  auto factory =
      kj::heap<WorkerFactory>(resources.v8System, resources.timer, resources.monotonicClock,
          resources.network, resources.entropySource, resources.fs, kj::mv(options),
          kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), config.size())
              .as<kj_rs::RustCopy>());
  // The server owns the factory for as long as it runs.
  auto server = new_in_process_server(kj::mv(factory), /*mock_internet=*/false);

  auto reports = co_await serve(*server);
  if (reports.errors.size() > 0) {
    // The config's errors are why the load failed, so a failure to close joins them rather than
    // taking their place.
    kj::Vector<kj::String> reported(reports.errors.size() + 1);
    for (auto& error: reports.errors) {
      reported.add(kj::mv(error));
    }
    KJ_TRY {
      co_await close_in_process_server(kj::mv(server));
    }
    KJ_CATCH(exception) {
      reported.add(kj::str("Closing the server then failed: ", exception.getDescription()));
    }
    kj::throwFatalException(loadFailed(source.path, reported));
  }
  co_return Started{.server = kj::mv(server), .warnings = kj::mv(reports.warnings)};
}

// A JSG error's message, for the console.
kj::StringPtr messageOf(const kj::Exception& exception) {
  return jsg::tunneledErrorType(exception.getDescription()).message;
}

// Writes a line to stderr, where the console writes its own messages.
void printLine(kj::StringPtr line) {
  fprintf(stderr, "%s\n", line.cStr());
}

}  // namespace

kj::Promise<kj::Own<ReplConfigServer>> ReplConfigServer::load(
    ReplServerContext context, ReplConfigSource source) {
  kj::Maybe<kj::Own<ReplConfigWatch>> watch;
  if (source.watch) {
    watch = context.loader.newWatch();
  }
  auto started = co_await start(context, source,
      watch.map([](kj::Own<ReplConfigWatch>& w) -> ReplConfigWatch& { return *w; }));
  auto self = kj::refcounted<ReplConfigServer>(
      context, kj::mv(source), kj::mv(started.server), kj::mv(started.warnings));
  KJ_IF_SOME(w, watch) {
    self->watchFor(kj::mv(w));
  }
  co_return self;
}

ReplConfigServer::ReplConfigServer(ReplServerContext context,
    ReplConfigSource source,
    ::rust::Box<InProcessServer> server,
    kj::Array<kj::String> warnings)
    : context(context),
      source(kj::mv(source)),
      server(kj::mv(server)),
      warnings(kj::mv(warnings)),
      reloading(kj::Promise<void>(kj::READY_NOW).fork()) {}

ReplConfigServer::~ReplConfigServer() noexcept(false) {
  // The REPL closes every server it loaded before it goes; a server dropped while running would
  // take its factory down without draining it.
  KJ_ASSERT(server == kj::none, "a REPL server was destroyed without being closed", source.path);
}

InProcessServer& ReplConfigServer::requireOpen() {
  KJ_IF_SOME(s, server) {
    if (!isClosed()) return *s;
  }
  JSG_FAIL_REQUIRE(Error, "The server for ", source.path, " was closed.");
}

kj::Rc<IoChannelFactory::SubrequestChannel> ReplConfigServer::service(
    kj::StringPtr name, kj::Maybe<kj::StringPtr> entrypoint, kj::Maybe<Frankenvalue> props) {
  auto ownProps = props.map([](Frankenvalue& p) { return kj::heap<Frankenvalue>(kj::mv(p)); });
  return subrequest_channel_into_kj(
      requireOpen().service(toRust(name), toRust(entrypoint), kj::mv(ownProps)));
}

kj::Rc<IoChannelFactory::SubrequestChannel> ReplConfigServer::socket(kj::StringPtr name) {
  return subrequest_channel_into_kj(requireOpen().socket(toRust(name)));
}

kj::Rc<IoChannelFactory::SubrequestChannel> ReplConfigServer::durableObject(
    const ReplActorAddress& address) {
  return subrequest_channel_into_kj(requireOpen().actor(toRust(address.service),
      toRust(address.className), toRust(address.name), toRust(address.id)));
}

kj::Promise<void> ReplConfigServer::reload() {
  return queueReload(ReportReloadOutcome::NO).addBranch();
}

kj::ForkedPromise<void> ReplConfigServer::queueReload(ReportReloadOutcome report) {
  auto reload = reloadAfter(reloading.addBranch(), report).fork();
  // `reloading` keeps the reload going whether or not anyone awaits it.
  reloading = reload.addBranch().catch_([](kj::Exception&&) {}).fork();
  return reload;
}

kj::Promise<void> ReplConfigServer::reloadAfter(
    kj::Promise<void> previous, ReportReloadOutcome report) {
  co_await previous;
  // The new server's isolates start on the event loop, not under the lock of the isolate that
  // asked for the reload.
  co_await kj::yield();
  if (report && isClosed()) {
    // Asked for by a change noticed just before the close.
    co_return;
  }
  KJ_TRY {
    co_await replaceServer();
    if (report) {
      printLine(kj::str("Reloaded ", source.path, ": a file it was loaded from changed."));
    }
  }
  KJ_CATCH(exception) {
    if (!report) {
      kj::throwFatalException(kj::mv(exception));
    }
    printLine(
        kj::str(source.path, " did not reload, and its server runs on: ", messageOf(exception)));
  }
}

kj::Promise<void> ReplConfigServer::replaceServer() {
  JSG_REQUIRE(!isClosed(), Error, "The server for ", source.path, " was closed.");
  kj::Maybe<kj::Own<ReplConfigWatch>> watch;
  if (source.watch) {
    watch = context.loader.newWatch();
  }
  kj::Maybe<Started> started;
  kj::Maybe<kj::Exception> failure;
  KJ_TRY {
    started = co_await start(context, source,
        watch.map([](kj::Own<ReplConfigWatch>& w) -> ReplConfigWatch& { return *w; }));
  }
  KJ_CATCH(exception) {
    failure = kj::mv(exception);
  }
  // Whether or not the load worked, the files it read are the ones to watch.
  KJ_IF_SOME(w, watch) {
    if (!isClosed()) {
      watchFor(kj::mv(w));
    }
  }
  KJ_IF_SOME(exception, failure) {
    kj::throwFatalException(kj::mv(exception));
  }

  // Closing waits for this reload, so the server is still there, and is closed after it.
  auto& next = KJ_ASSERT_NONNULL(started);
  auto previous = kj::mv(KJ_ASSERT_NONNULL(server));
  server = kj::mv(next.server);
  warnings = kj::mv(next.warnings);
  auto reason = kj::str("The server for ", source.path,
      " was reloaded. Get a new Fetcher from its service() or socket().");
  previous->revoke(toRust(reason));
  co_await close_in_process_server(kj::mv(previous));
}

void ReplConfigServer::watchFor(kj::Own<ReplConfigWatch> watch) {
  auto changed = watch->whenChanged();
  watching = changed.attach(kj::mv(watch))
                 .then([this]() {
    // The reload replaces this promise, once this has returned: `reloading` holds the reload.
    queueReload(ReportReloadOutcome::YES);
  }, [this](kj::Exception&& exception) {
    printLine(kj::str("Stopped watching the files of ", source.path, ": ", messageOf(exception)));
  }).eagerlyEvaluate(nullptr);
}

kj::Promise<void> ReplConfigServer::close() {
  // What waits for the close keeps the server until it is done: the REPL lets go of a server once
  // closing begins, and the JavaScript handles may be collected.
  KJ_IF_SOME(c, closing) {
    return c.addBranch().attach(kj::addRef(*this));
  }
  return closing.emplace(closeServer().fork()).addBranch().attach(kj::addRef(*this));
}

kj::Promise<void> ReplConfigServer::closeServer() {
  // The server's isolates end on the event loop, not under the lock of the isolate that asked for
  // the close.
  co_await kj::yield();
  watching = kj::none;
  // A reload under way finishes, and its server is the one closed. No reload starts after this
  // one: they refuse once closing has begun.
  co_await reloading.addBranch();
  KJ_IF_SOME(s, server) {
    // What the REPL holds of the server goes first: anything that still holds a request or a
    // channel would keep the factory alive.
    auto reason = kj::str("The server for ", source.path, " was closed.");
    s->revoke(toRust(reason));
    auto closed = close_in_process_server(kj::mv(s));
    server = kj::none;
    co_await closed;
  }
}

}  // namespace workerd::server
