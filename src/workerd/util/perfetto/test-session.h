// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <rust/cxx.h>

namespace workerd::perfetto_test {

bool compiled_in();

// Commits the calling thread's buffered events to the running session.
void flush_thread();

// Starts a session recording the "workerd" category into the file at `path`.
void start_session(::rust::Str path);

// Commits the calling thread's events, stops the session, and decodes the slice events in the
// file, as test_session.rs describes.
::rust::Vec<::rust::String> stop_session(::rust::Str path);

}  // namespace workerd::perfetto_test
