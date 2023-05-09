// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>
#include <kj/debug.h>
#include <kj/one-of.h>

namespace workerd::api {
  struct HibernatableSocketParams {
    // Event types and their corresponding parameters.

    struct Text {
      kj::String message;
    };

    struct Data {
      kj::Array<kj::byte> message;
    };

    struct Close {
      int code;
      kj::String reason;
      bool wasClean;
    };

    struct Error {
      kj::Exception error;
    };

    kj::OneOf<Text, Data, Close, Error> eventType;

    explicit HibernatableSocketParams(kj::String message): eventType(Text { kj::mv(message) }) {}
    explicit HibernatableSocketParams(kj::Array<kj::byte> message)
        : eventType(Data { kj::mv(message) }) {}
    explicit HibernatableSocketParams(int code, kj::String reason, bool wasClean)
        : eventType(Close { code, kj::mv(reason), wasClean }) {}
    explicit HibernatableSocketParams(kj::Exception e): eventType(Error { kj::mv(e) }) {}

    HibernatableSocketParams(HibernatableSocketParams&& other) = default;

    bool isCloseEvent() {
      return eventType.is<Close>();
    }
  };
}; // namespace workerd::api
