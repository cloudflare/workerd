// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn output_path_replaces_the_extension() {
    assert_eq!(
        output_path("workerd/api/r2-api.capnp"),
        "workerd/api/r2_api_serde_json.rs"
    );
    assert_eq!(output_path("foo.capnp"), "foo_serde_json.rs");
    assert_eq!(output_path("a-b/c-d"), "a-b/c_d_serde_json.rs");
}

#[test]
fn ffi_output_path_is_next_to_the_types() {
    assert_eq!(
        ffi::output_path("workerd/api/r2-api.capnp"),
        "workerd/api/r2_api_serde_json_ffi.rs"
    );
    assert_eq!(ffi::output_path("foo.capnp"), "foo_serde_json_ffi.rs");
}
