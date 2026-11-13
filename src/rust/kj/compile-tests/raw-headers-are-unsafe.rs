// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The bridge functions that return headers as a `KjOwn`, which has no lifetime, are unsafe:
// the headers refer to a table, and a shallow clone to its source.
use kj::http::ffi;

pub fn new(table: &ffi::HttpHeaderTable) {
    let _ = ffi::new_http_headers(table); //~ ERROR E0133
}

pub fn clone(headers: &ffi::HttpHeaders) {
    let _ = ffi::clone_shallow(headers); //~ ERROR E0133
}
