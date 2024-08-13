// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize-text.h>
#include <kj/debug.h>
#include <kj/list.h>
#include <kj/map.h>
#include <kj/refcount.h>
#include <kj/source-location.h>

namespace workerd {

// =======================================================================================
// KJ assert macros that support specifying a SourceLocation. These allow our test functions
// below to capture the caller's SourceLocation for use in errors, which is nice.
//
// TODO(cleanup): Move this to KJ!

#define KJ_REQUIRE_AT(cond, location, ...)                                                         \
  if (auto _kjCondition = ::kj::_::MAGIC_ASSERT << cond) {                                         \
  } else                                                                                           \
    for (::kj::_::Debug::Fault f(location.fileName, location.lineNumber,                           \
             ::kj::Exception::Type::FAILED, #cond, "_kjCondition," #__VA_ARGS__, _kjCondition,     \
             ##__VA_ARGS__);                                                                       \
         ; f.fatal())

#define KJ_FAIL_REQUIRE_AT(location, ...)                                                          \
  for (::kj::_::Debug::Fault f(location.fileName, location.lineNumber,                             \
           ::kj::Exception::Type::FAILED, nullptr, #__VA_ARGS__, ##__VA_ARGS__);                   \
       ; f.fatal())

#define KJ_REQUIRE_NONNULL_AT(value, location, ...)                                                \
  (*({                                                                                             \
    auto _kj_result = ::kj::_::readMaybe(value);                                                   \
    if (KJ_UNLIKELY(!_kj_result)) {                                                                \
      ::kj::_::Debug::Fault(location.fileName, location.lineNumber, ::kj::Exception::Type::FAILED, \
          #value " != nullptr", #__VA_ARGS__, ##__VA_ARGS__)                                       \
          .fatal();                                                                                \
    }                                                                                              \
    kj::mv(_kj_result);                                                                            \
  }))

#define KJ_ASSERT_AT KJ_REQUIRE_AT
#define KJ_FAIL_ASSERT_AT KJ_FAIL_REQUIRE_AT
#define KJ_ASSERT_NONNULL_AT KJ_REQUIRE_NONNULL_AT

#define KJ_LOG_AT(severity, location, ...)                                                         \
  for (bool _kj_shouldLog = ::kj::_::Debug::shouldLog(::kj::LogSeverity::severity); _kj_shouldLog; \
       _kj_shouldLog = false)                                                                      \
  ::kj::_::Debug::log(location.fileName, location.lineNumber, ::kj::LogSeverity::severity,         \
      #__VA_ARGS__, ##__VA_ARGS__)

// =======================================================================================
// Cap'n Proto mocking framework
//
// TODO(cleanup): Move this to Cap'n Proto!

const capnp::TextCodec TEXT_CODEC;

kj::String canonicalizeCapnpText(
    capnp::StructSchema schema, kj::StringPtr text, kj::Maybe<kj::StringPtr> capName = kj::none);

class MockClient: public capnp::DynamicCapability::Client {
public:
  using capnp::DynamicCapability::Client::Client;
  MockClient(capnp::DynamicCapability::Client&& client)
      : capnp::DynamicCapability::Client(kj::mv(client)) {}

  class ExpectedCall {
  public:
    ExpectedCall(capnp::RemotePromise<capnp::DynamicStruct> promise): promise(kj::mv(promise)) {}

    void expectReturns(
        kj::StringPtr resultsText, kj::WaitScope& ws, kj::SourceLocation location = {}) && {
      kj::String expectedResults = canonicalizeCapnpText(promise.getSchema(), resultsText);
      auto response = promise.wait(ws);
      auto actualResults = TEXT_CODEC.encode(response);
      KJ_ASSERT_AT(expectedResults == actualResults, location);
    }

    void expectThrows(kj::Exception::Type expectedType,
        kj::StringPtr expectedMessageSubstring,
        kj::WaitScope& ws,
        kj::SourceLocation location = {}) {
      promise
          .then([&](auto&&) {
        KJ_FAIL_ASSERT_AT(location, "expected call to throw exception but instead it returned",
            expectedType, expectedMessageSubstring);
      }, [&](kj::Exception&& e) {
        KJ_ASSERT_AT(e.getDescription().contains(expectedMessageSubstring), location,
            expectedMessageSubstring, e);
        KJ_ASSERT_AT(e.getType() == expectedType, location, e);
      }).wait(ws);
    }

  private:
    capnp::RemotePromise<capnp::DynamicStruct> promise;
  };

  ExpectedCall call(kj::StringPtr methodName, kj::StringPtr params) {
    auto req = newRequest(methodName);
    TEXT_CODEC.decode(params, req);
    return ExpectedCall(req.send());
  }
};

// Infrastructure to mock a capability!
//
// TODO(cleanup): This should obviously go in Cap'n Proto!
class MockServer: public kj::Refcounted {
  struct ReceivedCall;

public:
  MockServer(capnp::InterfaceSchema schema): schema(schema) {}

  template <typename T>
  struct Pair {
    kj::Own<MockServer> mock;
    typename T::Client client;
  };

  template <typename T>
  static Pair<T> make() {
    auto mock = kj::refcounted<MockServer>(capnp::Schema::from<T>());
    capnp::DynamicCapability::Client client = kj::heap<Server>(*mock);
    return {kj::mv(mock), client.as<T>()};
  }

  class ExpectedCall {
  public:
    ExpectedCall(ReceivedCall& received): maybeReceived(received) {
      received.expectedCall = this;
    }
    ExpectedCall(ExpectedCall&& other): maybeReceived(kj::mv(other.maybeReceived)) {
      KJ_IF_SOME(r, maybeReceived) r.expectedCall = *this;
    }
    ~ExpectedCall() noexcept(false) {
      KJ_IF_SOME(r, maybeReceived) {
        KJ_ASSERT(&KJ_ASSERT_NONNULL(r.expectedCall) == this);
        r.expectedCall = kj::none;
      }
    }

    ExpectedCall withParams(kj::StringPtr paramsText,
        kj::Maybe<kj::StringPtr> capName = kj::none,
        kj::SourceLocation location = {}) &&
        KJ_WARN_UNUSED_RESULT {
      // Expect that the call had the given parameters.

      auto& received = getReceived(location);

      kj::String expectedParams =
          canonicalizeCapnpText(received.method.getParamType(), paramsText, capName);

      auto actualParams = TEXT_CODEC.encode(received.context.getParams());
      KJ_ASSERT_AT(expectedParams == actualParams, location);

      return kj::mv(*this);
    }

    // Helper for cases where the received call is expected to invoke some callback capability.
    //
    // Expect that the params contain a field named `callbackName` whose type is an interface.
    // `func()` will be invoked and passed a `MockClient` representing this capability. It can
    // then invoke the callback as it seems fit.
    //
    // Note that it's explicitly OK if `func` captures a `WaitScope` and uses it. In this way,
    // the incoming call can be delayed from returning until the callback completes.
    template <typename Func>
        ExpectedCall useCallback(
            kj::StringPtr callbackName, Func&& func, kj::SourceLocation location = {}) &&
        KJ_WARN_UNUSED_RESULT {
      auto& received = getReceived(location);
      func(received.context.getParams().get(callbackName).as<capnp::DynamicCapability>());
      return kj::mv(*this);
    }

    // Causes the method to return the given result message, which is parsed from text.
    void thenReturn(kj::StringPtr message, kj::SourceLocation location = {}) && {
      auto& received = getReceived(location);
      TEXT_CODEC.decode(message, received.context.getResults());
      received.fulfiller.fulfill();
    }

    // Causes the method to return the given result message, which is parsed from text.
    // All capabilities in the result message will be filled in, with MockServer instances
    // returned in the hashmap.
    kj::HashMap<kj::String, kj::Own<MockServer>> thenReturnWithMocks(
        kj::StringPtr message, kj::SourceLocation location = {}) && {
      auto& received = getReceived(location);
      auto callResults = received.context.getResults();
      auto results = kj::HashMap<kj::String, kj::Own<MockServer>>();
      TEXT_CODEC.decode(message, callResults);
      for (const auto& field: received.method.getResultType().getFields()) {
        if (field.getType().isInterface()) {
          auto name = field.getProto().getName();
          auto mockServer = kj::refcounted<MockServer>(field.getType().asInterface());
          callResults.set(name, kj::heap<Server>(*mockServer));
          results.insert(kj::str(name), kj::mv(mockServer));
        }
      }

      received.fulfiller.fulfill();
      return kj::mv(results);
    }

    // Causes the method to throw an exception
    void thenThrow(kj::Exception&& e, kj::SourceLocation location = {}) && {
      auto& received = getReceived(location);
      received.fulfiller.reject(kj::mv(e));
    }

    // Return a new mock capability. The method result type is expected to contain a single
    // field with the given name whose type is an interface type. It will be filled in with a
    // new mock object, and the MockServer is returned in order to set further expectations.
    kj::Own<MockServer> returnMock(kj::StringPtr fieldName, kj::SourceLocation location = {}) && {
      auto& received = getReceived(location);
      auto field = received.method.getResultType().getFieldByName(fieldName);
      auto result = kj::refcounted<MockServer>(field.getType().asInterface());
      received.context.getResults().set(field, kj::heap<Server>(*result));
      received.fulfiller.fulfill();
      return result;
    }

    void expectCanceled(kj::SourceLocation location = {}) {
      KJ_ASSERT_AT(maybeReceived == kj::none, location, "call has not been canceled");
    }

  private:
    kj::Maybe<ReceivedCall&> maybeReceived;
    ReceivedCall& getReceived(kj::SourceLocation location) {
      return KJ_REQUIRE_NONNULL_AT(maybeReceived, location, "call was unexpectedly canceled");
    }
    friend struct ReceivedCall;
  };

  ExpectedCall expectCall(kj::StringPtr methodName,
      kj::WaitScope& waitScope,
      kj::SourceLocation location = {}) KJ_WARN_UNUSED_RESULT {
    auto expectedMethod = schema.getMethodByName(methodName);

    KJ_ASSERT_AT(
        waitForEvent(waitScope), location, "no method call was received when expected", methodName);

    KJ_ASSERT_AT(
        !dropped, location, "capability was dropped without making expected call", methodName);

    auto& received = receivedCalls.front();
    receivedCalls.remove(received);

    KJ_ASSERT_AT(received.method == expectedMethod, location,
        "a different method was called than expected", received.method.getProto().getName(),
        expectedMethod.getProto().getName());

    return ExpectedCall(received);
  }

  void expectDropped(kj::WaitScope& waitScope, kj::SourceLocation location = {}) {
    KJ_ASSERT_AT(waitForEvent(waitScope), location, "capability was not dropped when expected");
    KJ_ASSERT_AT(
        receivedCalls.empty(), location, receivedCalls.front().method.getProto().getName());

    KJ_ASSERT(dropped);  // should always be true if receivedCalls is empty
  }

  void expectNoActivity(kj::WaitScope& waitScope, kj::SourceLocation location = {}) {
    if (waitForEvent(waitScope)) {
      if (!receivedCalls.empty()) {
        KJ_FAIL_ASSERT_AT(location, "unexpected call received",
            receivedCalls.front().method.getProto().getName());
      }
      if (dropped) {
        KJ_FAIL_ASSERT_AT(location, "mock capability unexpectedly dropped");
      }
    }
  }

private:
  capnp::InterfaceSchema schema;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> waiter;

  struct ReceivedCall {
    ReceivedCall(kj::PromiseFulfiller<void>& fulfiller,
        MockServer& mock,
        capnp::InterfaceSchema::Method method,
        capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct> context)
        : fulfiller(fulfiller),
          mock(mock),
          method(method),
          context(kj::mv(context)) {
      mock.receivedCalls.add(*this);
      KJ_IF_SOME(w, mock.waiter) {
        w.get()->fulfill();
      }
    }
    ~ReceivedCall() noexcept(false) {
      if (link.isLinked()) {
        mock.receivedCalls.remove(*this);
      }
      KJ_IF_SOME(e, expectedCall) {
        e.maybeReceived = kj::none;
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(ReceivedCall);

    kj::PromiseFulfiller<void>& fulfiller;
    MockServer& mock;
    capnp::InterfaceSchema::Method method;
    capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct> context;
    kj::ListLink<ReceivedCall> link;

    kj::Maybe<ExpectedCall&> expectedCall;  // if one is attached
  };

  kj::List<ReceivedCall, &ReceivedCall::link> receivedCalls;
  bool dropped = false;

  bool waitForEvent(kj::WaitScope& waitScope) {
    if (receivedCalls.empty() && !dropped) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      waiter = kj::mv(paf.fulfiller);
      if (!paf.promise.poll(waitScope)) {
        waiter = kj::none;
        return false;
      }
      paf.promise.wait(waitScope);
    }
    return true;
  }

  class Server final: public capnp::DynamicCapability::Server {
  public:
    Server(MockServer& mock)
        : capnp::DynamicCapability::Server(mock.schema, {.allowCancellation = true}),
          mock(kj::addRef(mock)) {}
    ~Server() noexcept(false) {
      mock->dropped = true;
      KJ_IF_SOME(w, mock->waiter) {
        w.get()->fulfill();
      }
    }

    kj::Promise<void> call(capnp::InterfaceSchema::Method method,
        capnp::CallContext<capnp::DynamicStruct, capnp::DynamicStruct> context) override {
      return kj::newAdaptedPromise<void, ReceivedCall>(*mock, method, kj::mv(context));
    }

  private:
    kj::Own<MockServer> mock;
  };
};

// Wraps a "capnp struct literal". This actually just stringifies the arguments, adding enclosing
// parentheses. The nice thing about it, though, is that you don't have to escape quotes inside
// the literal.
#define CAPNP(...) ("(" #__VA_ARGS__ ")"_kj)

template <typename Schema, typename InitFunc>
kj::String Capnp(InitFunc func) {
  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<Schema>();
  func(builder);
  return TEXT_CODEC.encode(builder.asReader());
}

}  // namespace workerd
