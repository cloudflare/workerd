// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_macros::jsg_resource;

/// `pyodide-internal:internalJaeger` when the embedder traces nothing: an object with no
/// `traceId`, so `enterJaegerSpan()` (`src/pyodide/internal/jaeger.ts`) runs its callback outside
/// any span.
#[jsg_resource]
#[derive(Default)]
pub struct DisabledInternalJaeger;

#[jsg_resource]
impl DisabledInternalJaeger {}
