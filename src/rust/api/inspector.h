// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The native half of `node-internal:inspector` (see inspector.rs): a session connected to the
// isolate's own V8 inspector. Kept light, since the generated bridge header includes it; the
// implementation, which depends on workerd's I/O layer, is in inspector.c++.

#include <rust/cxx.h>

#include <kj/memory.h>

namespace v8 {
class Isolate;
}

namespace workerd::rust::api {

// A session connected to an isolate's V8 inspector, implemented in inspector.c++. Its destructor
// is virtual so that it can be destroyed from Rust without its definition; it must run with the
// isolate locked.
class InspectorSession {
 public:
  virtual ~InspectorSession() noexcept(false) = default;
};

// Whether this process runs Workers of more than one tenant, where no inspector session may be
// created.
bool inspector_is_multi_tenant_process();

// Whether the isolate, which must be locked, has a V8 inspector. The experimental
// `enable_nodejs_inspector_local_dev` flag creates one, as does an inspector policy that allows
// DevTools sessions (see Worker::Isolate::tryGetV8Inspector()).
bool inspector_isolate_has_v8_inspector(v8::Isolate* isolate);

// Connects a fully trusted session to the V8 inspector of the isolate, which must be locked with
// a context entered and have a V8 inspector, in a process that is not multi-tenant.
kj::Own<InspectorSession> inspector_session_connect(v8::Isolate* isolate);

// Dispatches a Chrome DevTools Protocol message (JSON) to the session. The responses and
// notifications the V8 inspector sends are buffered for inspector_session_take_messages(). Runs
// JavaScript for commands that evaluate code (e.g. `Runtime.evaluate`). The isolate must be locked
// with a context entered.
void inspector_session_dispatch(InspectorSession& session, ::rust::Str message);

// Returns the buffered responses and notifications, in the order the V8 inspector sent them.
::rust::Vec<::rust::String> inspector_session_take_messages(InspectorSession& session);

}  // namespace workerd::rust::api
