// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn a_udp_address_names_a_port_unless_the_system_chooses_one() {
    assert!(names_port("127.0.0.1:8080"));
    assert!(names_port("*:53"));
    assert!(names_port("[::1]:8080"));
    assert!(names_port("localhost:007"));
    // Port 0, however it is written, or none.
    assert!(!names_port("127.0.0.1:0"));
    assert!(!names_port("127.0.0.1:000"));
    assert!(!names_port("127.0.0.1"));
    assert!(!names_port("[::1]:0"));
    assert!(!names_port("[::1]"));
    // A bare IPv6 address has colons of its own.
    assert!(!names_port("::1"));
    assert!(!names_port("fe80::1:2"));
}

#[test]
fn a_jsg_errors_message_has_no_prefixes_and_no_type() {
    assert_eq!(message_of("jsg.Error: it broke"), "it broke");
    assert_eq!(message_of("jsg.TypeError: Bad: value"), "Bad: value");
    assert_eq!(message_of("remote.jsg.Error: far away"), "far away");
    assert_eq!(
        message_of("expected x == y; jsg.Error: not equal"),
        "not equal"
    );
    assert_eq!(message_of("jsg-internal.Error: hidden"), "hidden");
    // Any other description is its own message.
    assert_eq!(message_of("internal error"), "internal error");
}

#[test]
fn a_path_from_the_working_directory_resolves_dots_as_its_text_says() {
    let cwd = std::env::current_dir().expect("the working directory");
    assert_eq!(
        from_working_directory("a/./b/../c"),
        Some(cwd.join("a").join("c").display().to_string())
    );
    let absolute = cwd.join("x").display().to_string();
    assert_eq!(from_working_directory(&absolute), Some(absolute.clone()));
}
