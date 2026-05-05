// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/rust/worker/ffi.rs.h>

#include <kj-rs/kj-rs.h>

namespace workerd::rust::worker {

inline workerd::EventOutcome fromImpl(kj_rs::Rust*, workerd::rust::worker::EventOutcome outcome) {
  switch (outcome) {
    case workerd::rust::worker::EventOutcome::Unknown:
      return workerd::EventOutcome::UNKNOWN;
    case workerd::rust::worker::EventOutcome::Ok:
      return workerd::EventOutcome::OK;
    case workerd::rust::worker::EventOutcome::Exception:
      return workerd::EventOutcome::EXCEPTION;
    case workerd::rust::worker::EventOutcome::ExceededCpu:
      return workerd::EventOutcome::EXCEEDED_CPU;
    case workerd::rust::worker::EventOutcome::KillSwitch:
      return workerd::EventOutcome::KILL_SWITCH;
    case workerd::rust::worker::EventOutcome::DaemonDown:
      return workerd::EventOutcome::DAEMON_DOWN;
    case workerd::rust::worker::EventOutcome::ScriptNotFound:
      return workerd::EventOutcome::SCRIPT_NOT_FOUND;
    case workerd::rust::worker::EventOutcome::Canceled:
      return workerd::EventOutcome::CANCELED;
    case workerd::rust::worker::EventOutcome::ExceededMemory:
      return workerd::EventOutcome::EXCEEDED_MEMORY;
    case workerd::rust::worker::EventOutcome::LoadShed:
      return workerd::EventOutcome::LOAD_SHED;
    case workerd::rust::worker::EventOutcome::ResponseStreamDisconnected:
      return workerd::EventOutcome::RESPONSE_STREAM_DISCONNECTED;
    case workerd::rust::worker::EventOutcome::InternalError:
      return workerd::EventOutcome::INTERNAL_ERROR;
  }
}

inline workerd::WorkerInterface::ScheduledResult fromImpl(
    kj_rs::Rust*, workerd::rust::worker::ScheduledResult result) {
  return workerd::WorkerInterface::ScheduledResult{
    .retry = result.retry,
    .outcome = kj::from<kj_rs::Rust>(result.outcome),
  };
}

inline workerd::WorkerInterface::AlarmResult fromImpl(
    kj_rs::Rust*, workerd::rust::worker::AlarmResult result) {
  return workerd::WorkerInterface::AlarmResult{
    .retry = result.retry,
    .retryCountsAgainstLimit = result.retry_counts_against_limit,
    .outcome = kj::from<kj_rs::Rust>(result.outcome),
    .errorDescription = result.error_description.empty()
        ? kj::none
        : kj::Maybe<kj::String>(kj::str(result.error_description)),
  };
}

inline workerd::WorkerInterface::CustomEvent::Result fromImpl(
    kj_rs::Rust*, workerd::rust::worker::CustomEventResult result) {
  return workerd::WorkerInterface::CustomEvent::Result{
    .outcome = kj::from<kj_rs::Rust>(result.outcome),
  };
}

class RustWorkerInterface final: public workerd::WorkerInterface {
 public:
  using Impl = workerd::rust::worker::Wrapper;
  RustWorkerInterface(::rust::Box<Impl> impl): impl(kj::mv(impl)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    return impl->request(method, url.asBytes().as<kj_rs::Rust>(), headers, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& tunnel,
      kj::HttpConnectSettings settings) override {
    return impl->connect(host.asBytes().as<kj_rs::Rust>(), headers, connection, tunnel,
        kj::rust::HttpConnectSettings{
          .use_tls = settings.useTls,
          .tls_starter = settings.tlsStarter,
        });
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return impl->prewarm(url.asBytes().as<kj_rs::Rust>());
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    co_return kj::from<kj_rs::Rust>(
        co_await impl->run_scheduled(scheduledTime, cron.asBytes().as<kj_rs::Rust>()));
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    co_return kj::from<kj_rs::Rust>(co_await impl->run_alarm(scheduledTime, retryCount));
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    co_return kj::from<kj_rs::Rust>(co_await impl->custom_event(*event));
  }

  kj::Promise<bool> test() override {
    return impl->test();
  }

 private:
  ::rust::Box<Impl> impl;
};

inline kj::Own<workerd::WorkerInterface> fromImpl(
    kj_rs::Rust*, ::rust::Box<RustWorkerInterface::Impl> impl) {
  return kj::heap<RustWorkerInterface>(kj::mv(impl));
}

}  // namespace workerd::rust::worker
