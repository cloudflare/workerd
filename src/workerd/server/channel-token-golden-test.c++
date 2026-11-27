// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The handler against channel tokens that its C++ encoder wrote before the Rust codec
// (channel-token-codec/) replaced it, in channel-token-codec/golden-tokens.txt: it reads each,
// some as written, and writes some storage tokens, which have no IV, to the same bytes. The
// codec's own tests write every token again from its contents and IV.

#include "channel-token.h"

#include <capnp/message.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

using Usage = IoChannelFactory::ChannelTokenUsage;

kj::String strProps(const Frankenvalue& props) {
  capnp::MallocMessageBuilder message;
  props.threadSafeClone().toCapnp(message.getRoot<rpc::Frankenvalue>());
  return kj::str(message.getRoot<rpc::Frankenvalue>().asReader());
}

kj::Array<byte> expectSync(kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> variant) {
  return KJ_ASSERT_NONNULL(kj::mv(variant).tryGet<kj::Array<byte>>());
}

class MockSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  MockSubrequestChannel(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent,
      kj::Maybe<ChannelTokenHandler&> handler = kj::none)
      : serviceName(kj::str(serviceName)),
        entrypoint(entrypoint.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)),
        persistent(persistent),
        handler(handler) {}

  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Frankenvalue props;
  Persistent persistent;
  kj::Maybe<ChannelTokenHandler&> handler;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    return KJ_ASSERT_NONNULL(handler).encodeSubrequestChannelToken(usage, serviceName,
        entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }), props, persistent);
  }
};

class MockActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  MockActorClassChannel(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent)
      : serviceName(kj::str(serviceName)),
        entrypoint(entrypoint.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)),
        persistent(persistent) {}

  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Frankenvalue props;
  Persistent persistent;

  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    KJ_UNREACHABLE;
  }
};

class MockActorChannel final: public IoChannelFactory::ActorChannel {
 public:
  MockActorChannel(kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent)
      : namespaceKey(kj::str(namespaceKey)),
        id(kj::heapArray(id)),
        name(name.map([](kj::StringPtr s) { return kj::str(s); })),
        persistent(persistent) {}

  kj::String namespaceKey;
  kj::Array<byte> id;
  kj::Maybe<kj::String> name;
  Persistent persistent;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    KJ_UNREACHABLE;
  }
};

class MockResolver final: public ChannelTokenHandler::Resolver {
 public:
  // The handler that the subrequest channels it resolves encode their tokens with.
  kj::Maybe<ChannelTokenHandler&> handler;

  kj::Rc<IoChannelFactory::SubrequestChannel> resolveEntrypoint(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override {
    return kj::rc<MockSubrequestChannel>(
        serviceName, entrypoint, kj::mv(props), persistent, handler);
  }
  kj::Rc<IoChannelFactory::ActorClassChannel> resolveActorClass(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override {
    return kj::rc<MockActorClassChannel>(serviceName, entrypoint, kj::mv(props), persistent);
  }
  kj::Rc<IoChannelFactory::ActorChannel> resolveActor(kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent) override {
    return kj::rc<MockActorChannel>(namespaceKey, id, name, persistent);
  }
};

kj::String goldenText() {
  return kj::newDiskFilesystem()
      ->getCurrent()
      .openFile(kj::Path::parse("src/workerd/server/channel-token-codec/golden-tokens.txt"))
      ->readAllText();
}

// The token named `name` in golden-tokens.txt.
kj::Array<byte> golden(kj::StringPtr name) {
  auto text = goldenText();
  // Each line is `name usage type layout hex`; comments start with `#`.
  kj::StringPtr rest = text;
  while (rest.size() > 0) {
    auto lineEnd = rest.findFirst('\n').orDefault(rest.size());
    auto line = rest.first(lineEnd);
    rest = lineEnd < rest.size() ? rest.slice(lineEnd + 1) : ""_kj;
    if (line.size() == 0 || line[0] == '#') continue;
    auto nameEnd = KJ_ASSERT_NONNULL(line.findFirst(' '));
    if (line.first(nameEnd) != name) continue;
    auto hexStart = KJ_ASSERT_NONNULL(line.findLast(' ')) + 1;
    auto result = kj::decodeHex(line.slice(hexStart));
    KJ_ASSERT(!result.hadErrors, name);
    return kj::mv(result);
  }
  KJ_FAIL_ASSERT("no golden token", name);
}

struct Fixture {
  MockResolver resolver;
  ChannelTokenHandler handler{resolver, "golden-cluster-key"_kj};
  bool init = (resolver.handler = handler, true);
};

const byte ACTOR_ID[] = {0xde, 0xad, 0xbe, 0xef};

Frankenvalue propsWithCaps(ChannelTokenHandler& handler) {
  auto props = Frankenvalue::fromJson(kj::str("{\"a\": 1}"));
  props.setProperty(kj::str("nested"),
      Frankenvalue::fromCapability(7,
          kj::refcounted<MockSubrequestChannel>("nested-service", "NestedEntry"_kj,
              Frankenvalue::fromJson(kj::str("{\"inner\": true}")), Persistent::YES, handler)));
  props.setProperty(kj::str("plain"), Frankenvalue::fromJson(kj::str("[1, 2, 3]")));
  return props;
}

KJ_TEST("every golden token reads") {
  Fixture f;
  auto text = goldenText();
  size_t count = 0;
  kj::StringPtr rest = text;
  while (rest.size() > 0) {
    auto lineEnd = rest.findFirst('\n').orDefault(rest.size());
    auto line = kj::str(rest.first(lineEnd));
    rest = lineEnd < rest.size() ? rest.slice(lineEnd + 1) : ""_kj;
    if (line.size() == 0 || line[0] == '#') continue;

    // `name usage type layout hex`
    kj::Vector<kj::String> fields;
    kj::StringPtr remaining = line;
    while (remaining.size() > 0) {
      auto end = remaining.findFirst(' ').orDefault(remaining.size());
      fields.add(kj::str(remaining.first(end)));
      remaining = end < remaining.size() ? remaining.slice(end + 1) : ""_kj;
    }
    KJ_ASSERT(fields.size() == 5, line);
    auto usage = fields[1] == "rpc" ? Usage::RPC : Usage::STORAGE;
    auto token = kj::decodeHex(fields[4]);
    KJ_ASSERT(!token.hadErrors, fields[0]);

    if (fields[2] == "Subrequest") {
      f.handler.decodeSubrequestChannelToken(usage, token);
    } else if (fields[2] == "ActorClass") {
      f.handler.decodeActorClassChannelToken(usage, token);
    } else {
      KJ_ASSERT(fields[2] == "Rpc", line);
      f.handler.decodeRpcChannelToken(usage, token);
    }
    ++count;
  }
  KJ_EXPECT(count == 45);
}

KJ_TEST("golden service tokens read as written") {
  Fixture f;
  auto json = strProps(Frankenvalue::fromJson(kj::str("{\"foo\": 123}")));
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    auto prefix = usage == Usage::RPC ? "rpc"_kj : "storage"_kj;
    {
      auto channel =
          f.handler.decodeSubrequestChannelToken(usage, golden(kj::str(prefix, "-subrequest-json")))
              .downcast<MockSubrequestChannel>();
      KJ_EXPECT(channel->serviceName == "my-service");
      KJ_EXPECT(channel->entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }) ==
          "MyEntry"_kj);
      KJ_EXPECT(strProps(channel->props) == json);
      KJ_EXPECT(channel->persistent == Persistent::YES);
    }
    {
      auto channel = f.handler
                         .decodeActorClassChannelToken(
                             usage, golden(kj::str(prefix, "-actor-class-no-entrypoint")))
                         .downcast<MockActorClassChannel>();
      KJ_EXPECT(channel->serviceName == "my-service");
      KJ_EXPECT(channel->entrypoint == kj::none);
      KJ_EXPECT(strProps(channel->props) == json);
    }
    {
      auto channel =
          f.handler.decodeSubrequestChannelToken(usage, golden(kj::str(prefix, "-subrequest-caps")))
              .downcast<MockSubrequestChannel>();
      auto caps = channel->props.getCapTable();
      KJ_ASSERT(caps.size() == 1);
      auto& nested = KJ_ASSERT_NONNULL(kj::tryDowncast<MockSubrequestChannel>(*caps[0]));
      KJ_EXPECT(nested.serviceName == "nested-service");
      KJ_EXPECT(
          strProps(nested.props) == strProps(Frankenvalue::fromJson(kj::str("{\"inner\": true}"))));
    }
  }
}

KJ_TEST("golden actor tokens read as written") {
  Fixture f;
  struct Case {
    kj::StringPtr name;
    Usage usage;
    kj::Maybe<kj::StringPtr> actorName;
    Persistent persistent;
  };
  for (auto& c: kj::arr(Case{"rpc-actor-named", Usage::RPC, "my-actor"_kj, Persistent::YES},
           Case{"rpc-actor-unnamed", Usage::RPC, kj::none, Persistent::YES},
           Case{"rpc-actor-not-persistent", Usage::RPC, "my-actor"_kj, Persistent::NO},
           Case{"storage-actor-named", Usage::STORAGE, "my-actor"_kj, Persistent::YES},
           Case{"storage-actor-unnamed", Usage::STORAGE, kj::none, Persistent::YES})) {
    auto channel = f.handler.decodeSubrequestChannelToken(c.usage, golden(c.name))
                       .downcast<MockActorChannel>();
    KJ_EXPECT(channel->namespaceKey == "my-namespace", c.name);
    KJ_EXPECT(channel->id.asPtr() == kj::arrayPtr(ACTOR_ID), c.name);
    KJ_EXPECT(
        channel->name.map([](kj::String& s) -> kj::StringPtr { return s; }) == c.actorName, c.name);
    KJ_EXPECT(channel->persistent == c.persistent, c.name);
  }
}

KJ_TEST("golden restored tokens read as written") {
  Fixture f;
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    auto prefix = usage == Usage::RPC ? "rpc"_kj : "storage"_kj;
    auto channel = f.handler.decodeSubrequestChannelToken(
        usage, golden(kj::str(prefix, "-restored-subrequest")));
    // A restored channel writes its token again from the vendor it read and its restore argument.
    auto again = expectSync(channel->getTokenMaybeSync(usage));
    if (usage == Usage::STORAGE) {
      KJ_EXPECT(again == golden("storage-restored-subrequest"));
    }
    f.handler.decodeRpcChannelToken(usage, golden(kj::str(prefix, "-restored-rpc")));
  }
}

KJ_TEST("golden storage tokens are written to the same bytes") {
  Fixture f;
  auto json = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto withCaps = propsWithCaps(f.handler);
  auto empty = Frankenvalue::fromJson(kj::str("{}"));
  auto storage = Usage::STORAGE;

  KJ_EXPECT(expectSync(f.handler.encodeSubrequestChannelToken(storage, "my-service", "MyEntry"_kj,
                json, Persistent::YES)) == golden("storage-subrequest-json"));
  KJ_EXPECT(expectSync(f.handler.encodeActorClassChannelToken(storage, "my-service", kj::none, json,
                Persistent::YES)) == golden("storage-actor-class-no-entrypoint"));
  KJ_EXPECT(expectSync(f.handler.encodeSubrequestChannelToken(storage, "my-service", "MyEntry"_kj,
                withCaps, Persistent::YES)) == golden("storage-subrequest-caps"));
  KJ_EXPECT(f.handler.encodeActorChannelToken(storage, "my-namespace", ACTOR_ID, "my-actor"_kj,
                Persistent::YES) == golden("storage-actor-named"));
  KJ_EXPECT(f.handler.encodeActorChannelToken(storage, "my-namespace", ACTOR_ID, kj::none,
                Persistent::YES) == golden("storage-actor-unnamed"));
  auto vendor = expectSync(f.handler.encodeSubrequestChannelToken(
      storage, "vendor", "Vendor"_kj, empty, Persistent::YES));
  KJ_EXPECT(vendor == golden("storage-vendor"));
  KJ_EXPECT(expectSync(f.handler.encodeRestoredChannelToken(storage, ChannelToken::Type::SUBREQUEST,
                vendor.asPtr(), Frankenvalue::fromJson(kj::str("{\"restore\": \"me\"}")),
                Persistent::YES)) == golden("storage-restored-subrequest"));
  KJ_EXPECT(expectSync(f.handler.encodeRestoredChannelToken(storage, ChannelToken::Type::RPC,
                vendor.asPtr(), propsWithCaps(f.handler), Persistent::YES)) ==
      golden("storage-restored-rpc"));
}

}  // namespace
}  // namespace workerd::server
