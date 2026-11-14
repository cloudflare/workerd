// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Production code must not panic; test code is exempt via clippy.toml allow-*-in-tests.
#![deny(clippy::expect_used, clippy::panic, clippy::unreachable)]
#![deny(clippy::todo, clippy::unimplemented)]

//! Turns one invocation's spans and logs into OTLP/HTTP export requests.
//!
//! C++ reports each span's open, updates and close to a `SpanBuffer` (`bridge`), and gets back
//! encoded `ExportTraceServiceRequest`s to POST to a collector's `/v1/traces` as
//! `application/x-protobuf`. Likewise it reports logs to a `LogBuffer` and gets back
//! `ExportLogsServiceRequest`s for `/v1/logs`. Everything between is decided here: what a span
//! looks like in OTLP (`rules`), how spans and logs are batched (`buffer`, `logs`), and the wire
//! format (`proto`). Sending the request is left to the caller.

mod attributes;
mod bridge;
mod buffer;
mod logs;
mod proto;
mod rules;
