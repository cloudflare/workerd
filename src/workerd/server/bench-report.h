// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Helpers for `workerd bench`'s report: describing the machine and build, and rendering the
// report as text.

#include <workerd/io/bench.capnp.h>

#include <kj/string.h>

namespace workerd::server {

// Fills in the build mode, V8 version, CPU model, and frequency governor, and adds warnings for
// conditions that make results unreliable. The caller fills in the rest.
void fillBenchEnvironment(bench::BenchReport::Environment::Builder environment);

// Renders the report as text: a header describing the environment, then one table per group.
kj::String formatBenchReport(bench::BenchReport::Reader report);

// Formats a duration in nanoseconds with three significant figures and a unit, as in "12.3 ns" or
// "1.50 ms".
kj::String formatBenchDuration(double ns);

}  // namespace workerd::server
