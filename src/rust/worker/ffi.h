// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.h>
#include <workerd/rust/kj/ffi.h>
#include <workerd/rust/kj/http.rs.h>

#include <kj/compat/http.h>

namespace workerd::rust::worker {

using HttpMethod = kj::HttpMethod;
using HttpHeaders = kj::HttpHeaders;
using HttpServiceResponse = kj::HttpService::Response;
using AsyncInputStream = kj::AsyncInputStream;
using AsyncIoStream = kj::AsyncIoStream;
using ConnectResponse = kj::HttpService::ConnectResponse;

using CustomEvent = workerd::WorkerInterface::CustomEvent;

}  // namespace workerd::rust::worker
