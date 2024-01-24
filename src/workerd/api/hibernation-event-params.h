// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <cstdint>
#include <kj/common.h>
#include <kj/string.h>
#include <kj/debug.h>
#include <kj/one-of.h>

namespace workerd::api {
  // Event types and their corresponding parameters.
  struct HibernatableSocketParams {
    struct Text {
      kj::String message;
    };

    struct Data {
      kj::Array<kj::byte> message;
    };

    struct Close {
      uint16_t code;
      kj::String reason;
      bool wasClean;
    };

    struct Error {
      kj::Exception error;
    };

    kj::OneOf<Text, Data, Close, Error> eventType;
    kj::String websocketId;
    kj::Maybe<uint32_t> eventTimeoutMs;

    explicit HibernatableSocketParams(kj::String message, kj::String id)
        : eventType(Text { kj::mv(message) }), websocketId(kj::mv(id)) {}
    explicit HibernatableSocketParams(kj::Array<kj::byte> message, kj::String id)
        : eventType(Data { kj::mv(message) }), websocketId(kj::mv(id)) {}
    explicit HibernatableSocketParams(uint16_t code, kj::String reason, bool wasClean, kj::String id)
        : eventType(Close { code, kj::mv(reason), wasClean }), websocketId(kj::mv(id)) {}
    explicit HibernatableSocketParams(kj::Exception e, kj::String id)
        : eventType(Error { kj::mv(e) }), websocketId(kj::mv(id)) {}

    HibernatableSocketParams(HibernatableSocketParams&& other) = default;

    bool isCloseEvent() {
      return eventType.is<Close>();
    }

    void setTimeout(kj::Maybe<uint32_t> timeoutMs) {
      eventTimeoutMs = kj::mv(timeoutMs);
    }
  };
}; // namespace workerd::api
