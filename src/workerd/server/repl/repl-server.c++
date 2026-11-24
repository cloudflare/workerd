// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-server.h"

#include <workerd/io/worker.h>
#include <workerd/jsg/exception.h>
#include <workerd/server/factory/worker-factory.h>
#include <workerd/server/server/bridge.rs.h>
#include <workerd/server/workerd.capnp.h>

#include <kj-rs/kj-rs.h>

#include <capnp/serialize.h>

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

// `path`, a native path, from the working directory; none if it is not one, which the server
// reports.
kj::Maybe<kj::String> fromWorkingDirectory(const kj::Filesystem& fs, kj::StringPtr path) {
  kj::Maybe<kj::String> resolved;
  if (kj::runCatchingExceptions([&]() {
    resolved = fs.getCurrentPath().evalNative(path).toNativeString(true);
  }) != kj::none) {
    return kj::none;
  }
  return resolved;
}

// What a server of the config `words` would hold that no other server may (ReplServerClaims): the
// directory of each durable Durable Object namespace, as the server finds it, the path of the
// worker's `localDisk` service from the working directory, then the namespace's `uniqueKey`.
kj::Array<kj::String> exclusiveResources(
    const kj::Filesystem& fs, kj::ArrayPtr<const capnp::word> words) {
  capnp::ReaderOptions options;
  options.traversalLimitInWords = kj::maxValue;
  capnp::FlatArrayMessageReader message(words, options);
  auto services = message.getRoot<config::Config>().getServices();
  auto diskPath = [&](kj::StringPtr name) -> kj::Maybe<kj::StringPtr> {
    for (auto service: services) {
      if (service.getName() == name && service.isDisk() && service.getDisk().hasPath()) {
        return service.getDisk().getPath();
      }
    }
    return kj::none;
  };
  kj::Vector<kj::String> held;
  for (auto service: services) {
    if (!service.isWorker()) continue;
    auto worker = service.getWorker();
    auto storage = worker.getDurableObjectStorage();
    if (!storage.isLocalDisk()) continue;
    KJ_IF_SOME(path, diskPath(storage.getLocalDisk())) {
      KJ_IF_SOME(directory, fromWorkingDirectory(fs, path)) {
        for (auto ns: worker.getDurableObjectNamespaces()) {
          if (ns.isUniqueKey()) {
            held.add(kj::str("stores Durable Objects in ", directory, "/", ns.getUniqueKey()));
          }
        }
      }
    }
  }
  return held.releaseAsArray();
}

// A server built and not yet serving, and its claim.
struct Prepared {
  ::rust::Box<InProcessServer> server;
  kj::Own<ReplServerClaims::Claim> claim;
};

// Loads the config `source` names, with `watch` watching the files the load reads, claims what a
// server of it would hold, `except` aside (the claim of a server it is to replace), and builds
// that server. Throws a JSG error if the config does not load, or another server holds what it
// would.
Prepared prepare(const ReplServerContext& context,
    const ReplConfigSource& source,
    kj::Maybe<ReplConfigWatch&> watch,
    kj::Maybe<const ReplServerClaims::Claim&> except) {
  auto config = context.loader.load(source.path,
      source.constName.map([](const kj::String& name) { return name.asPtr(); }), watch);
  auto& resources = context.resources;
  auto claim =
      resources.claims.claim(source.path, exclusiveResources(resources.fs, config), except);
  auto options = kj::heap<WorkerFactory::Options>();
  options->loggingOptions = Worker::LoggingOptions(Worker::ConsoleMode::STDOUT);
  options->experimental = context.experimental;
  options->hostHeaderTable = context.replHeaderTable;
  auto words = config.asBytes();
  auto factory =
      kj::heap<WorkerFactory>(resources.v8System, resources.timer, resources.monotonicClock,
          resources.network, resources.entropySource, resources.fs, kj::mv(options),
          kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), config.size())
              .as<kj_rs::RustCopy>());
  // The server owns the factory for as long as it runs.
  return Prepared{.server = new_in_process_server(kj::mv(factory)), .claim = kj::mv(claim)};
}

// Serves `server`, the config `path`'s, and hands it back with the warnings it reported. Throws a
// JSG error listing the problems if it doesn't start, having closed it.
kj::Promise<Started> serveOrClose(kj::StringPtr path, ::rust::Box<InProcessServer> server) {
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
    kj::throwFatalException(loadFailed(path, reported));
  }
  co_return Started{.server = kj::mv(server), .warnings = kj::mv(reports.warnings)};
}

kj::Maybe<ReplConfigWatch&> watchOf(kj::Maybe<kj::Own<ReplConfigWatch>>& watch) {
  return watch.map([](kj::Own<ReplConfigWatch>& w) -> ReplConfigWatch& { return *w; });
}

// A JSG error's message without its type, for the console and for errors that report it.
kj::StringPtr messageOf(const kj::Exception& exception) {
  auto tunneled = jsg::tunneledErrorType(exception.getDescription());
  if (tunneled.isJsgError) {
    // As JSG reads it: the type, then ": ".
    KJ_IF_SOME(colon, tunneled.message.findFirst(':')) {
      auto text = tunneled.message.slice(colon + 1);
      while (text.startsWith(" ")) {
        text = text.slice(1);
      }
      return text;
    }
  }
  return tunneled.message;
}

// Writes a line to stderr, where the console writes its own messages.
void printLine(kj::StringPtr line) {
  fprintf(stderr, "%s\n", line.cStr());
}

}  // namespace

ReplServerClaims::Claim::Claim(
    ReplServerClaims& registry, kj::String path, kj::Array<kj::String> held)
    : registry(registry),
      path(kj::mv(path)),
      held(kj::mv(held)) {
  registry.claims.add(*this);
}

ReplServerClaims::Claim::~Claim() noexcept(false) {
  registry.claims.remove(*this);
}

kj::Own<ReplServerClaims::Claim> ReplServerClaims::claim(
    kj::StringPtr path, kj::Array<kj::String> held, kj::Maybe<const Claim&> except) {
  for (auto& other: claims) {
    KJ_IF_SOME(skipped, except) {
      if (&other == &skipped) continue;
    }
    for (auto& resource: held) {
      for (auto& otherResource: other.held) {
        JSG_REQUIRE(resource != otherResource, Error, path, " ", resource, ", as the server for ",
            other.path, " does. Close that server first.");
      }
    }
  }
  return kj::heap<Claim>(*this, kj::str(path), kj::mv(held));
}

kj::Promise<kj::Own<ReplConfigServer>> ReplConfigServer::load(
    ReplServerContext context, ReplConfigSource source) {
  kj::Maybe<kj::Own<ReplConfigWatch>> watch;
  if (source.watch) {
    watch = context.loader.newWatch();
  }
  auto prepared = prepare(context, source, watchOf(watch), kj::none);
  auto started = co_await serveOrClose(source.path, kj::mv(prepared.server));
  auto self = kj::refcounted<ReplConfigServer>(context, kj::mv(source), kj::mv(started.server),
      kj::mv(started.warnings), kj::mv(prepared.claim));
  KJ_IF_SOME(w, watch) {
    self->watchFor(kj::mv(w));
  }
  co_return self;
}

ReplConfigServer::ReplConfigServer(ReplServerContext context,
    ReplConfigSource source,
    ::rust::Box<InProcessServer> server,
    kj::Array<kj::String> warnings,
    kj::Own<ReplServerClaims::Claim> claim)
    : context(context),
      source(kj::mv(source)),
      server(kj::mv(server)),
      warnings(kj::mv(warnings)),
      reloading(kj::Promise<void>(kj::READY_NOW).fork()),
      claim(kj::mv(claim)) {}

ReplConfigServer::~ReplConfigServer() noexcept(false) {
  // The REPL closes every server it loaded before it goes; a server dropped while running would
  // take its factory down without draining it.
  KJ_DTOR_ASSERT(
      server == kj::none, "a REPL server was destroyed without being closed", source.path);
}

InProcessServer& ReplConfigServer::requireOpen() {
  JSG_REQUIRE(!isClosed(), Error, "The server for ", source.path, " was closed.");
  KJ_IF_SOME(s, server) {
    return *s;
  }
  JSG_REQUIRE(!replacing, Error, "The server for ", source.path, " is reloading.");
  JSG_FAIL_REQUIRE(Error, "Nothing serves ", source.path,
      ": its last reload failed to start the new server. Fix the config and reload().");
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
  kj::Maybe<kj::Own<ReplConfigWatch>> watch;
  if (isWatching()) {
    watch = context.loader.newWatch();
  }
  kj::Maybe<kj::Exception> failure;
  KJ_TRY {
    co_await replaceServer(watchOf(watch));
  }
  KJ_CATCH(exception) {
    failure = kj::mv(exception);
  }
  // Whether or not the reload worked, the files it read are the ones to watch. The watch starts
  // once the reload is done, so a change waits for it rather than queueing another behind it.
  KJ_IF_SOME(w, watch) {
    if (!isClosed()) {
      watchFor(kj::mv(w));
    }
  }

  KJ_IF_SOME(exception, failure) {
    if (server == kj::none && !isClosed()) {
      // The reload closed the server it was to replace.
      auto outcome =
          kj::str(source.path, " did not reload, and nothing serves it: ", messageOf(exception));
      if (!report) {
        kj::throwFatalException(JSG_KJ_EXCEPTION(FAILED, Error, outcome));
      }
      printLine(outcome);
    } else {
      if (!report) {
        kj::throwFatalException(kj::mv(exception));
      }
      printLine(
          kj::str(source.path, " did not reload, and its server runs on: ", messageOf(exception)));
    }
  } else if (report) {
    printLine(kj::str("Reloaded ", source.path, ": a file it was loaded from changed."));
  }
}

kj::Promise<void> ReplConfigServer::replaceServer(kj::Maybe<ReplConfigWatch&> watch) {
  JSG_REQUIRE(!isClosed(), Error, "The server for ", source.path, " was closed.");
  auto next = prepare(context, source, watch,
      claim.map([](kj::Own<ReplServerClaims::Claim>& c) -> const ReplServerClaims::Claim& {
    return *c;
  }));

  // The old server goes before the new one serves, and its claim stands until it has gone. Closing
  // waits for this reload, so nothing closes the old server meanwhile.
  replacing = true;
  KJ_DEFER(replacing = false);
  auto previousClaim = kj::mv(claim);
  claim = kj::mv(next.claim);
  // Nothing serves unless the new server starts. It never ran, so dropping it ends nothing.
  bool serving = false;
  KJ_DEFER(if (!serving) { claim = kj::none; });
  warnings = nullptr;
  KJ_IF_SOME(previous, server) {
    auto closing = kj::mv(previous);
    server = kj::none;
    auto reason = kj::str("The server for ", source.path,
        " was reloaded. Get a new one from its service(), socket() or durableObject().");
    KJ_IF_SOME(hold, context.beforeClose) {
      co_await hold();
    }
    closing->revoke(toRust(reason));
    co_await close_in_process_server(kj::mv(closing));
  }
  previousClaim = kj::none;
  auto started = co_await serveOrClose(source.path, kj::mv(next.server));
  server = kj::mv(started.server);
  warnings = kj::mv(started.warnings);
  serving = true;
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
  KJ_DEFER({
    claim = kj::none;
    closeDone = true;
  });
  // The server's isolates end on the event loop, not under the lock of the isolate that asked for
  // the close.
  co_await kj::yield();
  KJ_IF_SOME(hold, context.beforeClose) {
    co_await hold();
  }
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
