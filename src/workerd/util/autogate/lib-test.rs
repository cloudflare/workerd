// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

// One test: the gates are process-wide, and the tests of a crate run in parallel.
#[test]
fn enabled_lists_the_configured_gates_in_declaration_order() {
    let gates = jsg_test::set_autogates(&[]);
    assert_eq!(enabled().count(), 0);
    drop(gates);

    // Given out of order: WORKERD_AUTOGATES declares TEST_WORKERD first.
    let _gates = jsg_test::set_autogates(&["rust-compat-date", "test-workerd"]);
    assert_eq!(
        enabled().collect::<Vec<_>>(),
        ["test-workerd", "rust-compat-date"]
    );
}
