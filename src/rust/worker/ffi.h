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
using WorkerInterface = workerd::WorkerInterface;

// Shared result structs, defined in the generated bridge header (which includes this file). Forward
// declared here so the out-of-line shim declarations below can name them.
struct ScheduledResult;
struct AlarmResult;
struct CustomEventResult;

// Reverse-direction shims: call a C++ WorkerInterface from Rust. Methods returning shared result
// structs (run_scheduled / run_alarm / custom_event) are declared here but defined out-of-line in
// ffi.c++, since those structs are only complete in the generated bridge header.

inline kj::Promise<void> worker_request(WorkerInterface& worker,
    HttpMethod method,
    ::rust::Slice<const kj::byte> url,
    const HttpHeaders& headers,
    AsyncInputStream& requestBody,
    HttpServiceResponse& response) {
  auto urlStr = kj::str(kj::from<kj_rs::Rust>(url).asChars());
  co_await worker.request(method, urlStr, headers, requestBody, response);
}

inline kj::Promise<void> worker_connect(WorkerInterface& worker,
    ::rust::Slice<const kj::byte> host,
    const HttpHeaders& headers,
    AsyncIoStream& connection,
    ConnectResponse& response,
    kj::rust::HttpConnectSettings settings) {
  auto hostStr = kj::str(kj::from<kj_rs::Rust>(host).asChars());
  co_await worker.connect(hostStr, headers, connection, response,
      {
        .useTls = settings.use_tls,
        .tlsStarter = settings.tls_starter,
      });
}

inline kj::Promise<void> worker_prewarm(
    WorkerInterface& worker, ::rust::Slice<const kj::byte> url) {
  auto urlStr = kj::str(kj::from<kj_rs::Rust>(url).asChars());
  co_await worker.prewarm(urlStr);
}

inline kj::Promise<bool> worker_test(WorkerInterface& worker) {
  return worker.test();
}

kj::Promise<ScheduledResult> worker_run_scheduled(
    WorkerInterface& worker, int64_t scheduledTimeNanos, ::rust::Slice<const kj::byte> cron);
kj::Promise<AlarmResult> worker_run_alarm(
    WorkerInterface& worker, int64_t scheduledTimeNanos, uint32_t retryCount);
kj::Promise<CustomEventResult> worker_custom_event(
    WorkerInterface& worker, kj::Own<CustomEvent> event);

}  // namespace workerd::rust::worker
