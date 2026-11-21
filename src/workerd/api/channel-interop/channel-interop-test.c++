// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests the channel-interop crate in the requests of a real worker: a Rust resource
// (test_bridge.rs) takes, returns and holds Fetchers and DurableObjectClasses, and the Fetchers it
// makes send requests to the service of the channel they were made from.

#include <workerd/api/actor.h>
#include <workerd/api/channel-interop/test_bridge.rs.h>
#include <workerd/api/http.h>
#include <workerd/jsg/script.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api::channel_interop::test {
namespace {

// A numbered channel that may be passed to another worker, one that may not, and a numbered
// Durable Object class of each kind.
constexpr uint SERVICE_CHANNEL = 3;
constexpr uint LOCAL_CHANNEL = 4;
constexpr uint CLASS_CHANNEL = 5;
constexpr uint LOCAL_CLASS_CHANNEL = 6;
constexpr uint16_t PORT = 5432;

// The service a test channel reaches, which answers every request with "<name>: <url>".
class Service final: public WorkerInterface {
 public:
  explicit Service(kj::StringPtr name): name(name) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    auto body = kj::str(name, ": ", url);
    // Headers of the request's table, which the response must use.
    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();
    auto stream = response.send(200, "OK"_kj, responseHeaders, body.size());
    co_await stream->write(body.asBytes());
  }
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  kj::StringPtr name;
};

// A channel to the Service `name`.
class ServiceChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  ServiceChannel(kj::StringPtr name, bool transferable, kj::Maybe<bool&> destroyed = kj::none)
      : name(name),
        transferable(transferable),
        destroyed(destroyed) {}
  ~ServiceChannel() noexcept(false) {
    KJ_IF_SOME(d, destroyed) {
      d = true;
    }
  }

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    return kj::heap<Service>(name);
  }
  void requireAllowsTransfer() override {
    JSG_REQUIRE(transferable, DOMDataCloneError, "This test channel is not transferrable.");
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }

 private:
  kj::StringPtr name;
  bool transferable;
  kj::Maybe<bool&> destroyed;
};

class TestActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  explicit TestActorClassChannel(bool transferable): transferable(transferable) {}

  void requireAllowsTransfer() override {
    JSG_REQUIRE(transferable, DOMDataCloneError, "This test class is not transferrable.");
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }

 private:
  bool transferable;
};

// The worker's channels: SERVICE_CHANNEL and LOCAL_CHANNEL reach a Service, CLASS_CHANNEL is
// always `classChannel`, and LOCAL_CLASS_CHANNEL is a class that can't be transferred.
struct ChannelFactory final: public TestFixture::DummyIoChannelFactory {
  ChannelFactory(TimerChannel& timer, kj::Rc<IoChannelFactory::ActorClassChannel> classChannel)
      : DummyIoChannelFactory(timer),
        classChannel(kj::mv(classChannel)) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return getSubrequestChannelResolved(channel, kj::none, kj::none, Persistent::NO)
        ->startRequest(kj::mv(metadata));
  }
  kj::Rc<SubrequestChannel> getSubrequestChannelResolved(uint channel,
      kj::Maybe<Frankenvalue> props,
      kj::Maybe<VersionRequest> versionRequest,
      Persistent persistent) override {
    KJ_REQUIRE(channel == SERVICE_CHANNEL || channel == LOCAL_CHANNEL, channel);
    if (channel == SERVICE_CHANNEL) {
      return kj::rc<ServiceChannel>("service"_kj, true);
    } else {
      return kj::rc<ServiceChannel>("local"_kj, false);
    }
  }
  kj::Rc<ActorClassChannel> getActorClassResolved(
      uint channel, kj::Maybe<Frankenvalue> props, Persistent persistent) override {
    KJ_REQUIRE(channel == CLASS_CHANNEL || channel == LOCAL_CLASS_CHANNEL, channel);
    if (channel == CLASS_CHANNEL) {
      return classChannel.addRef();
    } else {
      return kj::rc<TestActorClassChannel>(false);
    }
  }

  kj::Rc<IoChannelFactory::ActorClassChannel> classChannel;
};

// Counts the subrequest clients wrapped for metrics, which every subrequest gets but in-house ones.
class CountingRequestObserver final: public RequestObserver {
 public:
  explicit CountingRequestObserver(uint& count): count(count) {}

  kj::Own<WorkerInterface> wrapSubrequestClient(
      kj::Own<WorkerInterface> client, CountSubrequest countSubrequest) override {
    ++count;
    return kj::mv(client);
  }

 private:
  uint& count;
};

class Harness {
 public:
  Harness()
      : classChannel(kj::rc<TestActorClassChannel>(true)),
        fixture(TestFixture::SetupParams{
          .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
              [this](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
                return kj::rc<ChannelFactory>(timer, classChannel.addRef());
              }),
          .requestObserverFactory =
              kj::Function<kj::Own<RequestObserver>()>([this]() -> kj::Own<RequestObserver> {
                return kj::refcounted<CountingRequestObserver>(metricsWrappedSubrequests);
              }),
        }) {}

  // Sets the globals the scripts use: `probe` (a Rust `Probe`); `service` and `local`, Fetchers
  // on SERVICE_CHANNEL and LOCAL_CHANNEL; `extended`, an ExtendedFetcher on SERVICE_CHANNEL;
  // `cls` and `localCls`, DurableObjectClasses on CLASS_CHANNEL and LOCAL_CLASS_CHANNEL; and
  // `serviceChannel`, SERVICE_CHANNEL.
  static void setGlobals(const TestFixture::Environment& env) {
    auto& js = env.js;
    auto global = jsg::JsObject(js.v8Context()->Global());
    auto probe = ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(probe_new(env.isolate))
                     .ToLocalChecked();
    global.set(js, "probe"_kj, jsg::JsValue(probe));
    global.set(js, "service"_kj,
        wrap(js, js.alloc<Fetcher>(SERVICE_CHANNEL, Fetcher::RequiresHostAndProtocol::YES)));
    global.set(js, "local"_kj,
        wrap(js, js.alloc<Fetcher>(LOCAL_CHANNEL, Fetcher::RequiresHostAndProtocol::YES)));
    global.set(js, "extended"_kj,
        wrap(js,
            js.alloc<ExtendedFetcher>(
                SERVICE_CHANNEL, Fetcher::RequiresHostAndProtocol::YES, PORT)));
    global.set(js, "cls"_kj, wrap(js, js.alloc<DurableObjectClass>(CLASS_CHANNEL)));
    global.set(js, "localCls"_kj, wrap(js, js.alloc<DurableObjectClass>(LOCAL_CLASS_CHANNEL)));
    global.set(js, "serviceChannel"_kj, js.num(SERVICE_CHANNEL));
  }

  template <typename T>
  static jsg::JsValue wrap(jsg::Lock& js, jsg::Ref<T> value) {
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<T>>());
    return jsg::JsValue(handler.wrap(js, kj::mv(value)));
  }

  template <typename T>
  static jsg::Ref<T> unwrap(jsg::Lock& js, jsg::JsValue value) {
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<T>>());
    return KJ_ASSERT_NONNULL(handler.tryUnwrap(js, value), value.toString(js));
  }

  static jsg::JsValue run(const TestFixture::Environment& env, kj::StringPtr source) {
    return jsg::NonModuleScript::compile(env.js, source, "channel-interop-test.js"_kj)
        .runAndReturn(env.js);
  }

  // `source`'s value, as a string.
  static kj::String eval(const TestFixture::Environment& env, kj::StringPtr source) {
    return run(env, source).toString(env.js);
  }

  // Waits for the promise `source` returns, which rejects if a check fails.
  static kj::Promise<void> awaitScript(const TestFixture::Environment& env, kj::StringPtr source) {
    return env.context.awaitJs(env.js, env.js.toPromise(run(env, source))).ignoreResult();
  }

  uint metricsWrappedSubrequests = 0;
  kj::Rc<IoChannelFactory::ActorClassChannel> classChannel;
  TestFixture fixture;
};

// Calls `call` in a script and returns what it threw, or "no exception".
kj::String thrownBy(const TestFixture::Environment& env, kj::StringPtr call) {
  return Harness::eval(
      env, kj::str("try { ", call, "; 'no exception' } catch (e) { `${e.name}: ${e.message}` }"));
}

KJ_TEST("a Fetcher argument accepts Fetchers and their subclasses, and nothing else") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    KJ_EXPECT(Harness::eval(env, "probe.takeFetcher(service)") == "true");
    KJ_EXPECT(Harness::eval(env, "probe.takeFetcher(extended)") == "true");
    // The same TypeError C++ JSG throws for a `jsg::Ref<Fetcher>` parameter.
    KJ_EXPECT(thrownBy(env, "probe.takeFetcher(cls)") ==
        "TypeError: Failed to execute 'takeFetcher' on 'Probe': parameter 1 is not of type "
        "'Fetcher'.");
    KJ_EXPECT(thrownBy(env, "probe.takeFetcher({})") ==
        "TypeError: Failed to execute 'takeFetcher' on 'Probe': parameter 1 is not of type "
        "'Fetcher'.");
    KJ_EXPECT(thrownBy(env, "probe.takeFetcher(probe)") ==
        "TypeError: Failed to execute 'takeFetcher' on 'Probe': parameter 1 is not of type "
        "'Fetcher'.");
    KJ_EXPECT(thrownBy(env, "probe.takeClass(service)") ==
        "TypeError: Failed to execute 'takeClass' on 'Probe': parameter 1 is not of type "
        "'DurableObjectClass'.");
    KJ_EXPECT(Harness::eval(env, "probe.takeClass(cls)") == "true");
    KJ_EXPECT(Harness::eval(env, "probe.hasFetcher(service)") == "true");
    KJ_EXPECT(Harness::eval(env, "probe.hasFetcher(undefined)") == "false");
    KJ_EXPECT(Harness::eval(env, "probe.hasFetcher()") == "false");
  });
}

KJ_TEST("a struct takes optional, nullable Fetchers and arrays of them") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    KJ_EXPECT(Harness::eval(env, "probe.describeOutbounds({})") == "outbound: default, tails: 0");
    KJ_EXPECT(Harness::eval(env, "probe.describeOutbounds({outbound: null})") ==
        "outbound: none, tails: 0");
    KJ_EXPECT(Harness::eval(env,
                  "probe.describeOutbounds({outbound: service, tails: [service, extended]})") ==
        "outbound: fetcher, tails: 2");
    KJ_EXPECT(thrownBy(env, "probe.describeOutbounds({tails: [service, 'x']})")
                  .startsWith("TypeError: "));
    // Worker Loader requires each to be transferable.
    KJ_EXPECT(thrownBy(env, "probe.describeOutbounds({outbound: local})") ==
        "DataCloneError: This test channel is not transferrable.");
  });
}

KJ_TEST("a Fetcher made from another's channel sends requests to the same service") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    return Harness::awaitScript(env, R"JS(
      (async () => {
        for (const original of [service, extended]) {
          const copy = probe.rewrap(original);
          if (copy === original) throw new Error("rewrap() returned its argument");
          if (Object.getPrototypeOf(copy) !== Object.getPrototypeOf(service)) {
            throw new Error("not a Fetcher");
          }
          const text = await (await copy.fetch("http://receiver/a")).text();
          if (text !== "service: http://receiver/a") throw new Error(`got ${text}`);
        }
        const transferable = probe.rewrapTransferable(service);
        const text = await (await transferable.fetch("http://receiver/b")).text();
        if (text !== "service: http://receiver/b") throw new Error(`got ${text}`);
        // Another channel's copy reaches that channel's service.
        const viaLocal = await (await probe.rewrap(local).fetch("http://receiver/c")).text();
        if (viaLocal !== "local: http://receiver/c") throw new Error(`got ${viaLocal}`);
      })()
    )JS"_kj);
  });
}

KJ_TEST("the Fetcher made from a channel holds that channel") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    Harness::setGlobals(env);
    kj::Rc<IoChannelFactory::SubrequestChannel> channel =
        kj::rc<ServiceChannel>("original"_kj, true);
    auto original = js.alloc<Fetcher>(env.context.addObject(channel.addRef()));
    jsg::JsObject(js.v8Context()->Global())
        .set(js, "original"_kj, Harness::wrap(js, kj::mv(original)));
    auto copy = Harness::unwrap<Fetcher>(js, Harness::run(env, "probe.rewrap(original)"));
    KJ_EXPECT(copy->getSubrequestChannel(env.context).get() == channel.get());
  });
}

KJ_TEST("a Fetcher made from a channel number sends requests to that channel's service") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    Harness::setGlobals(env);
    auto result = Harness::run(
        env, "const numbered = probe.fetcherForChannel(serviceChannel, true, false); numbered");
    KJ_EXPECT(Harness::eval(env, "probe.takeFetcher(numbered)") == "true");
    KJ_EXPECT(Harness::eval(env,
                  "Object.getPrototypeOf(numbered) === Object.getPrototypeOf(service)") == "true");
    // Bound to no IoContext, as a Fetcher on a numbered channel is, so a snapshot can carry it.
    auto numbered = Harness::unwrap<Fetcher>(js, result);
    KJ_EXPECT(numbered->snapshotClone() != kj::none);
    return Harness::awaitScript(env, R"JS(
      (async () => {
        const text = await (await numbered.fetch("http://receiver/numbered")).text();
        if (text !== "service: http://receiver/numbered") throw new Error(`got ${text}`);
      })()
    )JS"_kj);
  });
}

KJ_TEST("a Fetcher made from a channel number requires a host and protocol if asked to") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    return Harness::awaitScript(env, R"JS(
      (async () => {
        const strict = probe.fetcherForChannel(serviceChannel, true, false);
        const error = await strict.fetch("/path").then(() => "no exception", (e) => `${e}`);
        if (error !== "TypeError: Fetch API cannot load: /path") throw new Error(`got ${error}`);
        const lenient = probe.fetcherForChannel(serviceChannel, false, false);
        const text = await (await lenient.fetch("/path")).text();
        if (text !== "service: https://fake-host/path") throw new Error(`got ${text}`);
      })()
    )JS"_kj);
  });
}

KJ_TEST("an in-house Fetcher's subrequests are not wrapped for metrics") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    return Harness::awaitScript(env, R"JS(
      (async () => {
        const fetcher = probe.fetcherForChannel(serviceChannel, false, true);
        await (await fetcher.fetch("http://receiver/a")).text();
      })()
    )JS"_kj);
  });
  KJ_EXPECT(harness.metricsWrappedSubrequests == 0);
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    return Harness::awaitScript(env, R"JS(
      (async () => {
        const fetcher = probe.fetcherForChannel(serviceChannel, false, false);
        await (await fetcher.fetch("http://receiver/a")).text();
      })()
    )JS"_kj);
  });
  KJ_EXPECT(harness.metricsWrappedSubrequests == 1);
}

KJ_TEST("mapVirtualHost() from Rust registers the Fetcher's virtual host, as C++ does") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    Harness::setGlobals(env);
    auto hostAndPort = Harness::eval(env, "probe.mapVirtualHost(service, 5432)");
    auto fetcher = Harness::unwrap<Fetcher>(js, Harness::run(env, "service"));
    KJ_EXPECT(fetcher->hasConnectOverride());
    auto host = KJ_ASSERT_NONNULL(fetcher->getHostInternal());
    KJ_EXPECT(host.endsWith(".workers.alt"), host);
    KJ_EXPECT(hostAndPort == kj::str(host, ":5432"), hostAndPort);
    // Later calls return the same host and port, whatever their port.
    KJ_EXPECT(Harness::eval(env, "probe.mapVirtualHost(service, 1)") == hostAndPort);
    // So does C++.
    KJ_EXPECT(fetcher->mapVirtualHost(js, 1) == hostAndPort);
  });
}

KJ_TEST("a channel that can't be transferred is refused with the error C++ throws") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    KJ_EXPECT(thrownBy(env, "probe.rewrapTransferable(local)") ==
        "DataCloneError: This test channel is not transferrable.");
    KJ_EXPECT(thrownBy(env, "probe.rewrapClass(localCls)") ==
        "DataCloneError: This test class is not transferrable.");
    // Nor can one be kept past its request.
    KJ_EXPECT(thrownBy(env, "probe.stash(local)") ==
        "DataCloneError: This test channel is not transferrable.");
    KJ_EXPECT(stashed_count() == 0);
  });
}

KJ_TEST("a DurableObjectClass made from another's channel holds that channel") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    Harness::setGlobals(env);
    auto result = Harness::run(env, "const copy = probe.rewrapClass(cls); copy");
    KJ_EXPECT(Harness::eval(env, "copy !== cls") == "true");
    KJ_EXPECT(Harness::eval(env, "probe.takeClass(copy)") == "true");
    KJ_EXPECT(
        Harness::eval(env, "Object.getPrototypeOf(copy) === Object.getPrototypeOf(cls)") == "true");
    auto copy = Harness::unwrap<DurableObjectClass>(js, result);
    KJ_EXPECT(copy->getChannel(env.context).get() == harness.classChannel.get());
    // The copy belongs to this IoContext, as the C++ one made from an ActorClassChannel does.
    KJ_EXPECT(copy->snapshotClone() == kj::none);
  });
}

KJ_TEST("a Fetcher held by a Rust resource keeps its wrapper through garbage collection") {
  Harness harness;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Harness::setGlobals(env);
    Harness::run(env, R"JS(
      (() => {
        const held = probe.rewrap(service);
        held.tag = 1;
        probe.hold(held);
        if (probe.held() !== held) throw new Error("held() is not the Fetcher hold() was given");
      })();
    )JS"_kj);
    // The Rust resource holds the only reference to the Fetcher, which keeps the C++ object alive
    // whatever the tracing does; only tracing keeps its wrapper, with the expando, alive. The
    // first collection makes the reference a traced one, and the second collects the wrapper if
    // the trace leaves it unmarked.
    env.isolate->LowMemoryNotification();
    env.isolate->LowMemoryNotification();
    KJ_EXPECT(Harness::eval(env, "probe.held().tag") == "1");
    return Harness::awaitScript(env, R"JS(
      (async () => {
        const text = await (await probe.held().fetch("http://receiver/held")).text();
        if (text !== "service: http://receiver/held") throw new Error(`got ${text}`);
      })()
    )JS"_kj);
  });
}

KJ_TEST("a Fetcher and a channel can be dropped without the lock, after their request") {
  Harness harness;
  bool destroyed = false;
  harness.fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    Harness::setGlobals(env);
    kj::Rc<IoChannelFactory::SubrequestChannel> channel =
        kj::rc<ServiceChannel>("stashed"_kj, true, destroyed);
    jsg::JsObject(js.v8Context()->Global())
        .set(js, "stashed"_kj,
            Harness::wrap(js, js.alloc<Fetcher>(env.context.addObject(kj::mv(channel)))));
    Harness::run(env, "probe.stash(stashed)");
    KJ_EXPECT(stashed_count() == 2);
  });
  // The request and its IoContext are gone, but the stashed channel holds the channel.
  KJ_EXPECT(!destroyed);
  drop_stashed();
  KJ_EXPECT(stashed_count() == 0);
  KJ_EXPECT(destroyed);
  // The next lock releases the Fetcher, whose release without the lock was queued.
  harness.fixture.runInIoContext(
      [&](const TestFixture::Environment& env) { env.isolate->LowMemoryNotification(); });
}

}  // namespace
}  // namespace workerd::api::channel_interop::test
