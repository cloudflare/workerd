// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <kj-rs-http/http.rs.h>

#include <kj/compat/http.h>

static_assert(sizeof(kj::rust::HttpConnectSettings) == 16, "HttpConnectSettings size mismatch");
static_assert(alignof(kj::rust::HttpConnectSettings) == alignof(uint64_t),
    "HttpConnectSettings alignment mismatch");

namespace kj::rust {

// This stays out-of-line because HttpConnectSettings is defined in the generated cxx bridge
// header, and ffi.h cannot include that header without creating an include cycle.
//
// Implemented as a coroutine so that a service whose connect() throws *synchronously* (e.g. the
// default kj::HttpService::connect(), which KJ_UNIMPLEMENTEDs) produces a rejected promise
// rather than an exception escaping through the infallible promise-returning FFI shim.
kj::Promise<void> connect(HttpService& service,
    ::rust::Slice<const kj::byte> host,
    const HttpHeaders& headers,
    AsyncIoStream& connection,
    ConnectResponse& response,
    HttpConnectSettings settings) {
  auto strHost = kj::str(kj::from<kj_rs::Rust>(host).asChars());
  co_await service.connect(strHost, headers, connection, response,
      {
        .useTls = settings.use_tls,
        .tlsStarter = settings.tls_starter,
      });
}

// Out-of-line for the same reason: HttpHeaderEntry is defined in the generated cxx bridge header.
::rust::Vec<HttpHeaderEntry> get_all_headers(const HttpHeaders& headers) {
  ::rust::Vec<HttpHeaderEntry> result;
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    HttpHeaderEntry entry;
    // Header names are always ASCII (kj-http validates them), so this UTF-8 check cannot throw.
    entry.name = ::rust::String(name.begin(), name.size());
    entry.value.reserve(value.size());
    for (kj::byte b: value.asBytes()) {
      entry.value.push_back(b);
    }
    result.push_back(kj::mv(entry));
  });
  return result;
}

}  // namespace kj::rust
