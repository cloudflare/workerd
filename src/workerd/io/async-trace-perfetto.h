// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Writes async trace events (async-trace.h) to Perfetto, in the "workerd.async" category. With
// workerd: `--perfetto-trace=<path>=workerd,workerd.async`.
//
// Layout, under the process track:
//
// - One track per IoContext, named after the worker (and actor). It holds a `turn` slice per turn,
//   with a nested `lock` slice while the turn waited for its locks, and a `ctx_end` instant with
//   the context's stats.
// - One track per resource, under its context's track. A slice named after the resource spans
//   from creation until it settles (or is destroyed unsettled), and a `run` slice spans each of its
//   callbacks. Annotations are instants named by their key.
// - Flows: from a `create` instant on the creating callback's track to the new resource, and from
//   a resource's settlement to its next callback (the scheduling delay).

#include <workerd/io/async-trace.h>

namespace workerd {

// Whether a Perfetto session is recording the "workerd.async" category. False in builds without
// Perfetto.
bool isAsyncTracePerfettoEnabled();

// A sink for one IoContext's tracker. `worker` and `actor` name its track. Call only while
// isAsyncTracePerfettoEnabled(); events are dropped once the session stops.
kj::Own<AsyncTraceListener> newAsyncTracePerfettoSink(
    kj::StringPtr worker, kj::Maybe<kj::StringPtr> actor);

}  // namespace workerd
