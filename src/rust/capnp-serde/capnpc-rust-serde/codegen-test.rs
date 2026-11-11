// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn output_path_replaces_the_extension() {
    assert_eq!(
        output_path("workerd/api/r2-api.capnp"),
        "workerd/api/r2_api_serde.rs"
    );
    assert_eq!(output_path("foo.capnp"), "foo_serde.rs");
    assert_eq!(output_path("a-b/c-d"), "a-b/c_d_serde.rs");
}
