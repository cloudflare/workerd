// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "web-socket-data-message.h"

#include <kj/test.h>

namespace workerd::api {
namespace {

// =============================================================================
// Construction + basic accessors.
// =============================================================================

KJ_TEST("WebSocketDataMessage: text construction, isText/isBinary, size()") {
  WebSocketDataMessage msg(kj::str("hello"));
  KJ_EXPECT(msg.isText());
  KJ_EXPECT(!msg.isBinary());
  KJ_EXPECT(msg.size() == 5);
}

KJ_TEST("WebSocketDataMessage: binary construction, isText/isBinary, size()") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x01, 0x02, 0x03}));
  KJ_EXPECT(msg.isBinary());
  KJ_EXPECT(!msg.isText());
  KJ_EXPECT(msg.size() == 3);
}

KJ_TEST("WebSocketDataMessage: JSG-shape ctor lifts text from string-first OneOf") {
  // String-first matches our internal storage; the OneOf move ctor handles this directly.
  kj::OneOf<kj::String, kj::Array<kj::byte>> jsgInput = kj::str("from-jsg");
  WebSocketDataMessage msg(kj::mv(jsgInput));
  KJ_EXPECT(msg.isText());
  KJ_EXPECT(msg.size() == 8);
  KJ_IF_SOME(text, msg.tryGetText()) {
    KJ_EXPECT(text == "from-jsg"_kj);
  } else {
    KJ_FAIL_EXPECT("expected text variant after JSG-shape lift");
  }
}

KJ_TEST("WebSocketDataMessage: JSG-shape ctor lifts binary from string-first OneOf") {
  kj::OneOf<kj::String, kj::Array<kj::byte>> jsgInput = kj::heapArray<kj::byte>({0xab, 0xcd});
  WebSocketDataMessage msg(kj::mv(jsgInput));
  KJ_EXPECT(msg.isBinary());
  KJ_EXPECT(msg.size() == 2);
}

KJ_TEST("WebSocketDataMessage: JSG-shape ctor accepts binary-first OneOf via subset ctor") {
  // Binary-first matches `WebSocket::send`'s JSG parameter shape. `kj::OneOf`'s SFINAE-
  // enabled subset ctor reorders the variants during construction — no per-variant switch
  // required at the caller.
  kj::OneOf<kj::Array<kj::byte>, kj::String> jsgInput = kj::str("binary-first");
  WebSocketDataMessage msg(kj::mv(jsgInput));
  KJ_EXPECT(msg.isText());
  KJ_EXPECT(msg.size() == 12);
  KJ_IF_SOME(text, msg.tryGetText()) {
    KJ_EXPECT(text == "binary-first"_kj);
  } else {
    KJ_FAIL_EXPECT("expected text variant after binary-first lift");
  }

  kj::OneOf<kj::Array<kj::byte>, kj::String> jsgBin = kj::heapArray<kj::byte>({0x42, 0x43});
  WebSocketDataMessage binMsg(kj::mv(jsgBin));
  KJ_EXPECT(binMsg.isBinary());
  KJ_EXPECT(binMsg.size() == 2);
  KJ_IF_SOME(bytes, binMsg.tryGetBinary()) {
    KJ_EXPECT(bytes[0] == 0x42 && bytes[1] == 0x43);
  } else {
    KJ_FAIL_EXPECT("expected binary variant");
  }
}

KJ_TEST("WebSocketDataMessage: move construction preserves variant and contents") {
  WebSocketDataMessage src(kj::heapArray<kj::byte>({0xab, 0xcd}));
  WebSocketDataMessage dst(kj::mv(src));
  KJ_EXPECT(dst.isBinary());
  KJ_EXPECT(dst.size() == 2);
  KJ_IF_SOME(bytes, dst.tryGetBinary()) {
    KJ_EXPECT(bytes[0] == 0xab && bytes[1] == 0xcd);
  } else {
    KJ_FAIL_EXPECT("expected binary variant after move");
  }
}

// =============================================================================
// `asOneOf()` + `KJ_SWITCH_ONEOF` dispatch — verifies the borrowed-view OneOf works for
// switch dispatch on both Message and Ptr, with cases bound to `kj::StringPtr` /
// `kj::ArrayPtr<const byte>`.
// =============================================================================

KJ_TEST("WebSocketDataMessage::asOneOf: text variant dispatches via KJ_SWITCH_ONEOF") {
  WebSocketDataMessage msg(kj::str("hello"));

  bool sawText = false;
  KJ_SWITCH_ONEOF(msg.asOneOf()) {
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      KJ_EXPECT(text == "hello"_kj);
      sawText = true;
    }
    KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
      KJ_FAIL_EXPECT("expected text variant");
    }
  }
  KJ_EXPECT(sawText);
}

KJ_TEST("WebSocketDataMessage::asOneOf: binary variant dispatches via KJ_SWITCH_ONEOF") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x01, 0x02, 0x03}));

  bool sawBytes = false;
  KJ_SWITCH_ONEOF(msg.asOneOf()) {
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      KJ_FAIL_EXPECT("expected binary variant");
    }
    KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
      KJ_EXPECT(bytes.size() == 3);
      KJ_EXPECT(bytes[0] == 0x01 && bytes[1] == 0x02 && bytes[2] == 0x03);
      sawBytes = true;
    }
  }
  KJ_EXPECT(sawBytes);
}

KJ_TEST("WebSocketDataMessagePtr::asOneOf: KJ_SWITCH_ONEOF dispatch on Ptr") {
  kj::byte buf[] = {0xde, 0xad};
  WebSocketDataMessagePtr ptr{kj::ArrayPtr<const kj::byte>(buf)};

  bool sawBytes = false;
  KJ_SWITCH_ONEOF(ptr.asOneOf()) {
    KJ_CASE_ONEOF(text, kj::StringPtr) {
      KJ_FAIL_EXPECT("expected binary variant");
    }
    KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
      KJ_EXPECT(bytes.size() == 2);
      sawBytes = true;
    }
  }
  KJ_EXPECT(sawBytes);
}

// =============================================================================
// Borrowed-view inspection.
// =============================================================================

KJ_TEST("WebSocketDataMessage: tryGetText/tryGetBinary for present and absent variants") {
  WebSocketDataMessage textMsg(kj::str("ping"));
  KJ_IF_SOME(text, textMsg.tryGetText()) {
    KJ_EXPECT(text == "ping"_kj);
  } else {
    KJ_FAIL_EXPECT("expected to unwrap text variant");
  }
  KJ_EXPECT(textMsg.tryGetBinary() == kj::none);

  WebSocketDataMessage binMsg(kj::heapArray<kj::byte>({0x10, 0x20}));
  KJ_IF_SOME(bytes, binMsg.tryGetBinary()) {
    KJ_EXPECT(bytes.size() == 2);
    KJ_EXPECT(bytes[0] == 0x10 && bytes[1] == 0x20);
  } else {
    KJ_FAIL_EXPECT("expected to unwrap binary variant");
  }
  KJ_EXPECT(binMsg.tryGetText() == kj::none);
}

KJ_TEST("WebSocketDataMessage::asPtr: text round-trip preserves variant and contents") {
  WebSocketDataMessage msg(kj::str("round"));
  auto ptr = msg.asPtr();
  KJ_EXPECT(ptr.isText());
  KJ_EXPECT(ptr.size() == 5);
  KJ_IF_SOME(text, ptr.tryGetText()) {
    KJ_EXPECT(text == "round"_kj);
  } else {
    KJ_FAIL_EXPECT("expected text variant");
  }
}

KJ_TEST("WebSocketDataMessage::asPtr: binary round-trip preserves variant and contents") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0xab, 0xcd}));
  auto ptr = msg.asPtr();
  KJ_EXPECT(ptr.isBinary());
  KJ_EXPECT(ptr.size() == 2);
  KJ_IF_SOME(bytes, ptr.tryGetBinary()) {
    KJ_EXPECT(bytes[0] == 0xab && bytes[1] == 0xcd);
  } else {
    KJ_FAIL_EXPECT("expected binary variant");
  }
}

// =============================================================================
// `WebSocketDataMessagePtr` direct construction + inspection.
// =============================================================================

KJ_TEST("WebSocketDataMessagePtr: text construction, isText/isBinary, size()") {
  kj::StringPtr s = "hello"_kj;
  WebSocketDataMessagePtr ptr(s);
  KJ_EXPECT(ptr.isText());
  KJ_EXPECT(!ptr.isBinary());
  KJ_EXPECT(ptr.size() == 5);
}

KJ_TEST("WebSocketDataMessagePtr: binary construction, isText/isBinary, size()") {
  kj::byte buf[] = {0x01, 0x02, 0x03, 0x04};
  WebSocketDataMessagePtr ptr{kj::ArrayPtr<const kj::byte>(buf)};
  KJ_EXPECT(ptr.isBinary());
  KJ_EXPECT(!ptr.isText());
  KJ_EXPECT(ptr.size() == 4);
}

KJ_TEST("WebSocketDataMessagePtr::toOwned: text round-trip allocates owned copy") {
  kj::StringPtr borrowed = "owned"_kj;
  WebSocketDataMessagePtr ptr(borrowed);
  WebSocketDataMessage owned = ptr.toOwned();
  KJ_EXPECT(owned.isText());
  KJ_EXPECT(owned.size() == 5);
  KJ_IF_SOME(text, owned.tryGetText()) {
    KJ_EXPECT(text == "owned"_kj);
  } else {
    KJ_FAIL_EXPECT("expected text variant");
  }
}

KJ_TEST("WebSocketDataMessagePtr::toOwned: binary round-trip allocates owned copy") {
  kj::byte buf[] = {0xab, 0xcd};
  WebSocketDataMessagePtr ptr{kj::ArrayPtr<const kj::byte>(buf)};
  WebSocketDataMessage owned = ptr.toOwned();
  KJ_EXPECT(owned.isBinary());
  KJ_EXPECT(owned.size() == 2);
  KJ_IF_SOME(bytes, owned.tryGetBinary()) {
    KJ_EXPECT(bytes[0] == 0xab && bytes[1] == 0xcd);
    // Mutating the original buffer must not affect the owned copy.
    buf[0] = 0xff;
    KJ_EXPECT(bytes[0] == 0xab);
  } else {
    KJ_FAIL_EXPECT("expected binary variant");
  }
}

// =============================================================================
// Content-equality (`operator==`) on Message and Ptr.
// =============================================================================

KJ_TEST("operator==: text Message matches StringPtr with same content") {
  WebSocketDataMessage msg(kj::str("ping"));
  KJ_EXPECT(msg == "ping"_kj);
  // C++23 rewrite gives symmetric form for free.
  KJ_EXPECT("ping"_kj == msg);
}

KJ_TEST("operator==: text Message rejects different StringPtr content") {
  WebSocketDataMessage msg(kj::str("ping"));
  KJ_EXPECT(!(msg == "pong"_kj));
}

KJ_TEST("operator==: text Message rejects ArrayPtr (variant mismatch)") {
  WebSocketDataMessage msg(kj::str("ping"));
  // Bytes that would equal "ping" in ASCII; must still return false because the variants
  // don't match.
  kj::byte pingBytes[] = {0x70, 0x69, 0x6e, 0x67};
  KJ_EXPECT(!(msg == kj::ArrayPtr<const kj::byte>(pingBytes)));
}

KJ_TEST("operator==: binary Message matches ArrayPtr with same content") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x01, 0x02, 0x03}));
  kj::byte expected[] = {0x01, 0x02, 0x03};
  KJ_EXPECT(msg == kj::ArrayPtr<const kj::byte>(expected));
  KJ_EXPECT(kj::ArrayPtr<const kj::byte>(expected) == msg);
}

KJ_TEST("operator==: binary Message rejects different ArrayPtr content") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x01, 0x02, 0x03}));
  kj::byte different[] = {0x01, 0x02, 0x04};
  KJ_EXPECT(!(msg == kj::ArrayPtr<const kj::byte>(different)));
}

KJ_TEST("operator==: binary Message rejects ArrayPtr of different length") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x01, 0x02, 0x03}));
  kj::byte shorter[] = {0x01, 0x02};
  KJ_EXPECT(!(msg == kj::ArrayPtr<const kj::byte>(shorter)));
}

KJ_TEST("operator==: binary Message rejects StringPtr (variant mismatch)") {
  WebSocketDataMessage msg(kj::heapArray<kj::byte>({0x70, 0x69, 0x6e, 0x67}));
  KJ_EXPECT(!(msg == "ping"_kj));
}

KJ_TEST("operator==: Ptr matches its borrowed StringPtr/ArrayPtr") {
  kj::StringPtr text = "ping"_kj;
  WebSocketDataMessagePtr textPtr(text);
  KJ_EXPECT(textPtr == "ping"_kj);
  KJ_EXPECT(!(textPtr == "pong"_kj));

  kj::byte buf[] = {0xab, 0xcd};
  WebSocketDataMessagePtr binPtr{kj::ArrayPtr<const kj::byte>(buf)};
  kj::byte expected[] = {0xab, 0xcd};
  KJ_EXPECT(binPtr == kj::ArrayPtr<const kj::byte>(expected));
  kj::byte different[] = {0xab, 0xce};
  KJ_EXPECT(!(binPtr == kj::ArrayPtr<const kj::byte>(different)));
}

// =============================================================================
// `sendVia` over a real `kj::WebSocket` pipe.
// =============================================================================

KJ_TEST("WebSocketDataMessagePtr::sendVia dispatches text frame to kj::WebSocket") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto pipe = kj::newWebSocketPipe();
  WebSocketDataMessagePtr ptr(kj::StringPtr("hello"_kj));

  auto sendPromise = ptr.sendVia(*pipe.ends[0]);
  auto received = pipe.ends[1]->receive().wait(waitScope);
  sendPromise.wait(waitScope);

  KJ_ASSERT(received.is<kj::String>());
  KJ_EXPECT(received.get<kj::String>() == "hello"_kj);
}

KJ_TEST("WebSocketDataMessagePtr::sendVia dispatches binary frame to kj::WebSocket") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto pipe = kj::newWebSocketPipe();
  kj::byte buf[] = {0x01, 0x02, 0x03};
  WebSocketDataMessagePtr ptr{kj::ArrayPtr<const kj::byte>(buf)};

  auto sendPromise = ptr.sendVia(*pipe.ends[0]);
  auto received = pipe.ends[1]->receive().wait(waitScope);
  sendPromise.wait(waitScope);

  KJ_ASSERT(received.is<kj::Array<kj::byte>>());
  auto& bytes = received.get<kj::Array<kj::byte>>();
  KJ_EXPECT(bytes.size() == 3);
  KJ_EXPECT(bytes[0] == 0x01 && bytes[1] == 0x02 && bytes[2] == 0x03);
}

KJ_TEST("WebSocketDataMessage::sendVia delegates to kj::WebSocket via asPtr") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto pipe = kj::newWebSocketPipe();
  WebSocketDataMessage msg(kj::str("delegated"));

  auto sendPromise = msg.sendVia(*pipe.ends[0]);
  auto received = pipe.ends[1]->receive().wait(waitScope);
  sendPromise.wait(waitScope);

  KJ_ASSERT(received.is<kj::String>());
  KJ_EXPECT(received.get<kj::String>() == "delegated"_kj);
}

}  // namespace
}  // namespace workerd::api
