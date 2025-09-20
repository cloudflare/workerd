// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/kj/http.rs.h>

#include <kj/compat/http.h>

namespace kj::rust {
kj::Promise<void> connect(HttpService& service,
    ::rust::Str host,
    const HttpHeaders& headers,
    AsyncIoStream& connection,
    ConnectResponse& response,
    HttpConnectSettings settings) {
  return service.connect(kj::str(host), headers, connection, response,
      {
        .useTls = settings.use_tls,
        .tlsStarter = settings.tls_starter,
      });
}

}  // namespace kj::rust
