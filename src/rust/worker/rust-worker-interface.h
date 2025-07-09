// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.h>

#include <kj/async.h>

#include <memory>

// Include CXX types
#include "workerd/rust/worker/lib.rs.h"

#include <kj-rs/kj-rs.h>

namespace workerd {

WorkerInterface::ScheduledResult fromRust(rust::worker::ScheduledResult result) {
  KJ_UNIMPLEMENTED();
}

WorkerInterface::AlarmResult fromRust(rust::worker::AlarmResult result) {
  KJ_UNIMPLEMENTED();
}

WorkerInterface::CustomEvent::Result fromRust(rust::worker::CustomEventResult result) {
  KJ_UNIMPLEMENTED();
}

}

namespace workerd::rust::worker {

template <typename T>
class RustWorkerInterfaceWrapper final: public workerd::WorkerInterface {
 public:
  explicit RustWorkerInterfaceWrapper(T t): t_(kj::mv(t)) {}

  /// Destructor
  ~RustWorkerInterfaceWrapper() = default;

  // WorkerInterface implementation
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    return t_->rust_worker_request(method, url.as<kj_rs::Rust>(), headers, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    return t_->rust_worker_connect(host.as<kj_rs::Rust>(), headers, connection, response, settings);
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return t_->rust_worker_prewarm(url.as<kj_rs::Rust>());
  }

  kj::Promise<workerd::WorkerInterface::ScheduledResult> runScheduled(
      kj::Date scheduledTime, kj::StringPtr cron) override {
    co_return fromRust(
      co_await t_->rust_worker_run_scheduled(scheduledTime, cron.as<kj_rs::Rust>()));
  }

  kj::Promise<workerd::WorkerInterface::AlarmResult> runAlarm(
      kj::Date scheduledTime, uint32_t retryCount) override {
    co_return fromRust(co_await t_->rust_worker_run_alarm(scheduledTime, retryCount));
  }

  kj::Promise<bool> test() override {
    return t_->rust_worker_test();
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    co_return fromRust(co_await t_->rust_worker_custom_event(*event));
  }

 private:
  T t_;
};

}  // namespace workerd::rust::worker
