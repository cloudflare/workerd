// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust async trait analogue of WorkerInterface
//!
//! This module provides a Rust async trait interface that mirrors the C++ WorkerInterface
//! from `src/workerd/io/worker-interface.h`. The implementation uses CXX opaque types
//! to represent C++ classes and provides async trait methods for all WorkerInterface
//! functionality.

#[cxx::bridge(namespace = "workerd::rust::worker")]
mod ffi {
    // Forward declarations and includes
    unsafe extern "C++" {
        include!("src/rust/worker/worker-bridge.h");
    }

    // Opaque C++ types using the bridge header aliases
    unsafe extern "C++" {
        include!("memory");

        type HttpMethod;
        type HttpHeaders;
        type AsyncInputStream;
        type AsyncIoStream;
        type HttpServiceResponse;
        type ConnectResponse;
        type HttpConnectSettings;
        type Date;
        type StringPtr;
        type TaskSet;
        type IoContext_IncomingRequest;
        type Frankenvalue;
        type HttpOverCapnpFactory;
        type EventDispatcherClient;
        type CustomEvent;
    }

    // Shared types between Rust and C++
    #[derive(Debug, Clone)]
    struct ScheduledResult {
        retry: bool,
        outcome: u32, // EventOutcome as u32
    }

    #[derive(Debug, Clone)]
    struct AlarmResult {
        retry: bool,
        retry_counts_against_limit: bool,
        outcome: u32, // EventOutcome as u32
    }

    #[derive(Debug, Clone)]
    struct CustomEventResult {
        outcome: u32, // EventOutcome as u32
    }

    // Opaque Rust types exposed to C++
    extern "Rust" {
        type RustWorkerInterface;

        // Factory function to create wrapper from BoxedWorkerInterface
        fn create_rust_worker_interface_wrapper() -> Box<RustWorkerInterface>;

        // Factory function to create exception worker
        fn create_exception_worker(error_message: &str) -> Box<RustWorkerInterface>;

        // WorkerInterface methods that will be called from C++
        async unsafe fn rust_worker_request<'a>(
            self: &'a mut RustWorkerInterface,
            method: &'a HttpMethod,
            url: &'a str,
            headers: &'a HttpHeaders,
            request_body: Pin<&'a mut AsyncInputStream>,
            response: Pin<&'a mut HttpServiceResponse>,
        ) -> Result<()>;

        async unsafe fn rust_worker_connect<'a>(
            self: &'a mut RustWorkerInterface,
            host: &'a str,
            headers: &'a HttpHeaders,
            connection: Pin<&'a mut AsyncIoStream>,
            response: Pin<&'a mut ConnectResponse>,
            settings: Pin<&'a mut HttpConnectSettings>,
        ) -> Result<()>;

        async unsafe fn rust_worker_prewarm<'a>(
            self: &'a mut RustWorkerInterface,
            url: &'a str,
        ) -> Result<()>;

        async unsafe fn rust_worker_run_scheduled<'a>(
            self: &'a mut RustWorkerInterface,
            scheduled_time: &'a Date,
            cron: &'a str,
        ) -> Result<ScheduledResult>;

        async unsafe fn rust_worker_run_alarm<'a>(
            self: &'a mut RustWorkerInterface,
            scheduled_time: &'a Date,
            retry_count: u32,
        ) -> Result<AlarmResult>;

        async unsafe fn rust_worker_test<'a>(self: &'a mut RustWorkerInterface) -> Result<bool>;

        async unsafe fn rust_worker_custom_event<'a>(
            self: &'a mut RustWorkerInterface,
            event: Pin<&'a mut CustomEvent>,
        ) -> Result<CustomEventResult>;
    }
}

pub mod interface;
pub mod types;
pub mod wrapper;

// Re-export the main types for convenience
pub use interface::BoxedWorkerInterface;
pub use interface::PromisedWorkerInterface;
pub use interface::WorkerInterface;
pub use interface::WorkerInterfaceError;
pub use interface::from_exception;
pub use types::ALARM_RETRY_MAX_TRIES;
pub use types::ALARM_RETRY_START_SECONDS;
pub use types::AsyncResult;
pub use types::AsyncVoidResult;
pub use wrapper::*;

// Type aliases for convenience
pub type Error = Box<dyn std::error::Error>;
pub type Result<T> = std::result::Result<T, Error>;

// Re-export CXX FFI types
pub use ffi::AlarmResult;
pub use ffi::AsyncInputStream;
pub use ffi::AsyncIoStream;
pub use ffi::ConnectResponse;
pub use ffi::CustomEvent;
pub use ffi::CustomEventResult;
pub use ffi::Date;
pub use ffi::EventDispatcherClient;
pub use ffi::Frankenvalue;
pub use ffi::HttpConnectSettings;
pub use ffi::HttpHeaders;
pub use ffi::HttpMethod;
pub use ffi::HttpOverCapnpFactory;
pub use ffi::HttpServiceResponse;
pub use ffi::IoContext_IncomingRequest;
pub use ffi::ScheduledResult;
pub use ffi::StringPtr;
pub use ffi::TaskSet;
