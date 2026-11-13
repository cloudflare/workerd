// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! W3C `tracestate` parsing for span links, using the `opentelemetry` crate's `TraceState`.

use std::str::FromStr;

use opentelemetry::trace::TraceState;

#[cxx::bridge(namespace = "workerd::trace_state")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    extern "Rust" {
        /// Parses a `tracestate` value and returns its normalized header form: list members
        /// joined by `,`, without optional whitespace or empty members. Returns an empty string
        /// when the value is not UTF-8, is rejected by `TraceState`, or has no list members.
        fn parse_tracestate(value: &[u8]) -> String;
    }
}

fn parse_tracestate(value: &[u8]) -> String {
    std::str::from_utf8(value)
        .ok()
        .and_then(|value| TraceState::from_str(value).ok())
        .map(|state| state.header())
        .unwrap_or_default()
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
