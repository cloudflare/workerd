// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The native half of the TypeScript performance.now() and performance.timeOrigin
// (src/per_isolate/performance/). These declarations deliberately mention no
// performance types, so the per-isolate bootstrap can reach them without
// depending on performance.h.

// Performance's FunctionTemplate, for the signature on the bootstrap's
// Performance natives (see "THE C++ CONTRACT" in
// src/per_isolate/performance/AGENTS.md).
v8::Local<v8::FunctionTemplate> getPerformanceTemplateForBootstrap(jsg::Lock& js);

// The CppHeapPointerTag range of Performance wrappers, which
// performanceNowForBootstrap() requires.
v8::CppHeapPointerTagRange getPerformanceTagRangeForBootstrap(jsg::Lock& js);

// Performance::now() on `instance`, which keeps the Spectre-safe clock
// (dateNow()) and the perf-event accounting native. `instance` must already
// have passed the signature check. Like JSG's own dispatch, it aborts if the
// wrapper's tag is outside `tagRange`, which only an inconsistent wrapper can
// cause.
double performanceNowForBootstrap(
    jsg::Lock& js, v8::Local<v8::Object> instance, v8::CppHeapPointerTagRange tagRange);

// utils.getPerformancePrototype(): Performance.prototype, without creating the
// lazily allocated globalThis.performance singleton. The Performance class is
// only a global under the performance compat flags, so this is the only way the
// bootstrap can reach the prototype in every configuration.
jsg::JsObject getPerformancePrototypeForBootstrap(jsg::Lock& js);

}  // namespace workerd::api
