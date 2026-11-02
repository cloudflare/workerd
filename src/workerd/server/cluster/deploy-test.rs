// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::check_name;

#[test]
fn deployment_names_are_restricted() {
    for name in ["app", "my-app_2", "A", &"a".repeat(64)] {
        assert!(check_name(name).is_ok(), "{name}");
    }
    for name in [
        "",
        "-app",
        "_app",
        "a.b",
        "a/b",
        "catalog.json",
        &"a".repeat(65),
    ] {
        assert!(check_name(name).is_err(), "{name}");
    }
}
