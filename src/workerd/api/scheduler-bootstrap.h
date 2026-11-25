// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// scheduler.wait() for the per-isolate bootstrap (utils.schedulerWait), which has converted the
// delay to a number and read `signal` from the options: the promise that Scheduler::wait()
// returns for `delay` and, unless `signal` is undefined, that signal.
//
// Throws the TypeError that JSG reports for the `signal` field of WaitOptions if `signal` is
// neither undefined nor an AbortSignal, and what Scheduler::wait() throws outside of a request.
jsg::JsValue schedulerWaitForBootstrap(jsg::Lock& js, double delay, jsg::JsValue signal);

}  // namespace workerd::api
