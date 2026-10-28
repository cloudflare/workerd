// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::Harness;

#[test]
fn compatibility_flags_reach_the_rust_realm() {
    let harness = Harness::with_flags(|mut flags| {
        flags.set_node_js_compat(true);
        flags.set_fetch_refuses_unknown_protocols(true);
    });
    harness.run_in_context(|lock, _| {
        let flags = lock.feature_flags();
        assert!(flags.get_node_js_compat());
        assert!(flags.get_fetch_refuses_unknown_protocols());
        assert!(!flags.get_node_js_compat_v2());
        assert!(!flags.get_set_to_string_tag());
        Ok(())
    });

    Harness::new().run_in_context(|lock, _| {
        let flags = lock.feature_flags();
        assert!(!flags.get_node_js_compat());
        assert!(!flags.get_fetch_refuses_unknown_protocols());
        Ok(())
    });
}

#[test]
fn python_workers_makes_the_cpp_global_prototype_read_only() {
    let code = "Object.getOwnPropertyDescriptor(globalThis.constructor, 'prototype').writable";
    Harness::new().run_in_context(|lock, ctx| {
        assert!(ctx.eval::<bool>(lock, code).unwrap());
        Ok(())
    });

    let harness = Harness::with_flags(|mut flags| flags.set_python_workers(true));
    harness.run_in_context(|lock, ctx| {
        assert!(!ctx.eval::<bool>(lock, code).unwrap());
        Ok(())
    });
}

/// A second guard while one is alive would replace its configuration and then discard it.
#[test]
#[should_panic(expected = "an Autogates guard is already alive")]
fn autogate_guards_do_not_nest() {
    let _outer = crate::set_autogates(&[]);
    let _inner = crate::set_autogates(&["jsg-integer-enforce-range"]);
}

/// Dropping the guard lets the next one be made.
#[test]
fn autogate_guards_follow_each_other() {
    drop(crate::set_autogates(&["jsg-integer-enforce-range"]));
    let _again = crate::set_autogates(&[]);
}
