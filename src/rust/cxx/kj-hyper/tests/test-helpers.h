// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>

namespace kj_hyper_test {

// A non-owning kj::Own of the test's service, one per call, since kj::HttpService is called
// concurrently and the Rust adapter (kj::http::CxxService) takes one kj::Own each.
inline kj::Own<kj::HttpService> share_service(kj::HttpService* service) {
  return kj::attachRef(*service);
}

}  // namespace kj_hyper_test
