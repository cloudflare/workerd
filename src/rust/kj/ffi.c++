// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/kj/http.rs.h>

#include <kj/compat/http.h>

static_assert(sizeof(kj::rust::HttpConnectSettings) == 16, "HttpConnectSettings size mismatch");
static_assert(alignof(kj::rust::HttpConnectSettings) == alignof(uint64_t), "HttpConnectSettings alignment mismatch");

namespace kj::rust {
kj::Promise<void> connect(HttpService& service,
    ::rust::Slice<const kj::byte> host,
    const HttpHeaders& headers,
    AsyncIoStream& connection,
    ConnectResponse& response,
    HttpConnectSettings settings) {
  auto strHost = kj::str(kj::from<kj_rs::Rust>(host).asChars());
  return service.connect(strHost, headers, connection, response,
      {
        .useTls = settings.use_tls,
        .tlsStarter = settings.tls_starter,
      });
}

}  // namespace kj::rust
