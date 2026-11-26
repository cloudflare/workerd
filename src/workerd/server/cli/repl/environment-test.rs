// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::compatibility_flags;
use super::is_identifier_chain;

fn strings(flags: &[&str]) -> Vec<String> {
    flags.iter().map(|&flag| flag.to_owned()).collect()
}

#[test]
fn adds_the_repls_flags_after_the_users() {
    assert_eq!(
        compatibility_flags(&strings(&["fetch_refuses_unknown_protocols"]), false),
        strings(&[
            "fetch_refuses_unknown_protocols",
            "nodejs_compat",
            "unhandled_rejection_after_microtask_checkpoint",
        ])
    );
}

#[test]
fn leaves_out_a_flag_the_user_takes_a_position_on() {
    for flag in [
        "nodejs_compat",
        "no_nodejs_compat",
        "nodejs_compat_v2",
        "no_nodejs_compat_v2",
    ] {
        assert_eq!(
            compatibility_flags(&strings(&[flag]), false),
            strings(&[flag, "unhandled_rejection_after_microtask_checkpoint"])
        );
    }
    assert_eq!(
        compatibility_flags(
            &strings(&["no_unhandled_rejection_after_microtask_checkpoint"]),
            false
        ),
        strings(&[
            "no_unhandled_rejection_after_microtask_checkpoint",
            "nodejs_compat",
        ])
    );
}

#[test]
fn adds_extra_handlers_only_with_experimental() {
    assert_eq!(
        compatibility_flags(&[], true),
        strings(&[
            "nodejs_compat",
            "unhandled_rejection_after_microtask_checkpoint",
            "service_binding_extra_handlers",
        ])
    );
    // Naming a flag twice is an error.
    assert_eq!(
        compatibility_flags(&strings(&["service_binding_extra_handlers"]), true),
        strings(&[
            "service_binding_extra_handlers",
            "nodejs_compat",
            "unhandled_rejection_after_microtask_checkpoint",
        ])
    );
}

#[test]
fn identifier_chains() {
    for chain in ["a", "a.b", "$a._b.c1", "globalThis"] {
        assert!(is_identifier_chain(chain), "{chain}");
    }
    for other in [
        "", ".", "a.", ".a", "a..b", "1a", "a.1b", "a()", "a[0]", "a b", "'a'",
    ] {
        assert!(!is_identifier_chain(other), "{other}");
    }
}
