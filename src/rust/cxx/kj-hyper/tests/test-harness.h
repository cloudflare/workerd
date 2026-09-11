// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Shared scaffolding for the kj-hyper C++ integration tests (hyper-server-test, hyper-client-test,
// hyper-tls-test). Only genuinely-identical setup/helpers live here; anything that differs per
// suite (the TestHttpService bodies, ensureTokioInitialized, the per-suite fixtures) stays inline
// in each test file.

#pragma once

#include <kj-rs-io/async-io.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/vector.h>

#include <cstring>

namespace kj_hyper_test {

// These tests drive the tokio-backed KJ event loop (kj-rs-tokio + kj-rs-io), matching workerd
// under the rust I/O backend: hyper's accept/dial/connection/pump tasks are spawned onto
// the KJ thread's per-thread current_thread runtime, so a plain native kj event loop cannot
// drive them. Exposes the members the fixtures use with kj::AsyncIoContext's spelling.
struct TokioTestIo {
  kj_rs_io::TokioAsyncIoContext ctx = kj_rs_io::setupTokioAsyncIo();
  kj::AsyncIoProvider* provider = ctx.provider.get();
  kj::WaitScope& waitScope = *ctx.waitScope;
};

inline bool asciiEqualsIgnoreCase(kj::StringPtr a, kj::StringPtr b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    if (ca != cb) return false;
  }
  return true;
}

// Collect all values of a header by name (case-insensitive), in order.
inline kj::Vector<kj::String> collectHeader(const kj::HttpHeaders& headers, kj::StringPtr name) {
  kj::Vector<kj::String> result;
  headers.forEach([&](kj::StringPtr headerName, kj::StringPtr value) {
    if (asciiEqualsIgnoreCase(headerName, name)) {
      result.add(kj::str(value));
    }
  });
  return result;
}

inline kj::Maybe<kj::String> getHeader(const kj::HttpHeaders& headers, kj::StringPtr name) {
  auto all = collectHeader(headers, name);
  if (all.size() == 0) return kj::none;
  return kj::mv(all[0]);
}

inline constexpr size_t LARGE_BODY_SIZE = 8 * 1024 * 1024;    // response streaming test
inline constexpr size_t LARGE_UPLOAD_SIZE = 4 * 1024 * 1024;  // request streaming test
inline constexpr size_t CHUNK_SIZE = 64 * 1024;

inline kj::byte patternByte(uint64_t offset) {
  return static_cast<kj::byte>(offset % 251);
}

// Serves a WebSocket by echoing messages; a Close message is echoed back (same code and reason)
// and ends the session.
inline kj::Promise<void> wsEcho(kj::WebSocket& ws) {
  for (;;) {
    auto message = co_await ws.receive();
    KJ_SWITCH_ONEOF(message) {
      KJ_CASE_ONEOF(text, kj::String) {
        co_await ws.send(text.asArray());
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        co_await ws.send(data.asPtr());
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        co_await ws.close(close.code, close.reason);
        co_return;
      }
    }
  }
}

// Header table + custom header ids for the tests. Field declaration order guarantees the ids are
// registered before the table is built.
struct HeaderIds {
  kj::HttpHeaderTable::Builder builder;
  kj::HttpHeaderId xEcho = builder.add("X-Echo");
  kj::HttpHeaderId xBinary = builder.add("X-Binary");
  kj::HttpHeaderId xEchoBack = builder.add("X-Echo-Back");
  kj::HttpHeaderId xBinaryResp = builder.add("X-Binary-Resp");
  kj::HttpHeaderId xUrl = builder.add("X-Url");
  kj::Own<kj::HttpHeaderTable> table = builder.build();
};

// A ConnectionReceiver wrapper that counts accepted connections, to observe keep-alive reuse.
class CountingReceiver final: public kj::ConnectionReceiver {
 public:
  explicit CountingReceiver(kj::Own<kj::ConnectionReceiver> inner): inner(kj::mv(inner)) {}

  uint count = 0;

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    return inner->accept().then([this](kj::Own<kj::AsyncIoStream> stream) {
      ++count;
      return kj::mv(stream);
    });
  }
  uint getPort() override {
    return inner->getPort();
  }

 private:
  kj::Own<kj::ConnectionReceiver> inner;
};

// ---------------------------------------------------------------------------------------
// Raw-socket WebSocket wire helpers: frame composition/parsing and HTTP-head reads, for the
// wire-level fidelity tests.

// Composes a WebSocket frame; `maskKey` of kj::none means unmasked.
inline kj::Array<kj::byte> makeFrame(bool fin,
    kj::uint opcode,
    kj::ArrayPtr<const kj::byte> payload,
    kj::Maybe<kj::FixedArray<kj::byte, 4>> maskKey = kj::none,
    bool rsv1 = false) {
  kj::Vector<kj::byte> frame;
  frame.add(static_cast<kj::byte>((fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) | opcode));
  kj::byte maskBit = maskKey == kj::none ? 0x00 : 0x80;
  if (payload.size() < 126) {
    frame.add(static_cast<kj::byte>(maskBit | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.add(static_cast<kj::byte>(maskBit | 126));
    frame.add(static_cast<kj::byte>(payload.size() >> 8));
    frame.add(static_cast<kj::byte>(payload.size() & 0xFF));
  } else {
    frame.add(static_cast<kj::byte>(maskBit | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.add(static_cast<kj::byte>((uint64_t(payload.size()) >> shift) & 0xFF));
    }
  }
  KJ_IF_SOME(key, maskKey) {
    frame.addAll(kj::arrayPtr(key.begin(), 4));
    for (size_t i = 0; i < payload.size(); i++) {
      frame.add(static_cast<kj::byte>(payload[i] ^ key[i % 4]));
    }
  } else {
    frame.addAll(payload);
  }
  return frame.releaseAsArray();
}

struct RawFrame {
  bool fin;
  bool rsv1;
  kj::uint opcode;
  kj::Array<kj::byte> payload;
};

// Reads one WebSocket frame off the raw stream, unmasking if needed.
inline RawFrame readFrame(kj::AsyncIoStream& stream, kj::WaitScope& waitScope) {
  auto readExact = [&](kj::ArrayPtr<kj::byte> out) {
    auto n = stream.tryRead(out.begin(), out.size(), out.size()).wait(waitScope);
    KJ_ASSERT(n == out.size(), "connection closed mid-frame");
  };
  kj::byte head[2];
  readExact(kj::arrayPtr(head, 2));
  bool fin = (head[0] & 0x80) != 0;
  bool rsv1 = (head[0] & 0x40) != 0;
  kj::uint opcode = head[0] & 0x0F;
  bool masked = (head[1] & 0x80) != 0;
  uint64_t len = head[1] & 0x7F;
  if (len == 126) {
    kj::byte ext[2];
    readExact(kj::arrayPtr(ext, 2));
    len = (uint64_t(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    kj::byte ext[8];
    readExact(kj::arrayPtr(ext, 8));
    len = 0;
    for (auto b: ext) len = (len << 8) | b;
  }
  kj::byte maskKey[4] = {0, 0, 0, 0};
  if (masked) {
    readExact(kj::arrayPtr(maskKey, 4));
  }
  auto payload = kj::heapArray<kj::byte>(len);
  if (len > 0) readExact(payload);
  if (masked) {
    for (size_t i = 0; i < payload.size(); i++) {
      payload[i] ^= maskKey[i % 4];
    }
  }
  return RawFrame{fin, rsv1, opcode, kj::mv(payload)};
}

// Reads an HTTP head (through "\r\n\r\n") from the raw stream.
inline kj::String readHttpHead(kj::AsyncIoStream& stream, kj::WaitScope& waitScope) {
  kj::Vector<char> collected;
  char c;
  while (true) {
    auto n = stream.tryRead(&c, 1, 1).wait(waitScope);
    KJ_ASSERT(n == 1, "connection closed while reading HTTP head");
    collected.add(c);
    if (collected.size() >= 4 && memcmp(collected.end() - 4, "\r\n\r\n", 4) == 0) {
      break;
    }
  }
  return kj::str(collected.releaseAsArray());
}

}  // namespace kj_hyper_test
