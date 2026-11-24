// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// The C++ side of the mimetype crate's bridge (ffi.rs).

#include <workerd/util/strings.h>

#include <rust/cxx.h>

#include <kj/map.h>
#include <kj/string.h>

namespace workerd::rust::mimetype {

// Receives the parameters the Rust parser finds, in order: each name as written in the input
// (a slice of it, valid as long as the input is), and each value without quotes or escapes (valid
// only during the call). A name can repeat.
class ParamSink {
 public:
  virtual void add(::rust::Slice<const uint8_t> name, ::rust::Slice<const uint8_t> value) = 0;

  static kj::ArrayPtr<const char> chars(::rust::Slice<const uint8_t> bytes) {
    return kj::arrayPtr(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

 protected:
  ~ParamSink() = default;
};

// Collects parameters as MimeType stores them: the name lowercased, and the first value of a name
// kept.
class MimeParamSink final: public ParamSink {
 public:
  kj::HashMap<kj::String, kj::String> params;

  void add(::rust::Slice<const uint8_t> name, ::rust::Slice<const uint8_t> value) override {
    params.upsert(toLower(chars(name)), kj::heapString(chars(value)), [](auto&, auto&&) {});
  }
};

}  // namespace workerd::rust::mimetype
