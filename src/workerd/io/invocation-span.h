// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The user span of one Worker invocation, as an embedder that exports user spans itself records it
// (see BaseTracer::setStartInvocationSpanFunc()). The span is named after the handler type, and its
// attributes follow OTel semantic conventions where one exists and otherwise the names the
// streaming tail worker's OTel conversion uses, so both produce the same attributes.

#include <workerd/io/outcome.capnp.h>
#include <workerd/io/trace.h>

namespace workerd {

// Names `span` after the event and records the event's attributes on it.
void describeInvocationSpan(SpanBuilder& span, const tracing::EventInfo& info);

// Records the status of the invocation's HTTP response.
void setInvocationResponseStatus(SpanBuilder& span, uint statusCode);

// Records the method a JS RPC invocation called.
void setInvocationRpcMethod(SpanBuilder& span, kj::StringPtr methodName);

// Records how the invocation ended.
void setInvocationOutcome(
    SpanBuilder& span, EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime);

}  // namespace workerd
