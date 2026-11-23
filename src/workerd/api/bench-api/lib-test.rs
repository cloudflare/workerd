// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

fn describe(code: &str) -> Vec<u8> {
    let mut description = Vec::new();
    Harness::new().run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, code).unwrap();
        description = describe_exception(lock, value).unwrap();
        Ok(())
    });
    description
}

/// A lone surrogate is kept as WTF-8, as C++ JSG's `kj::String` keeps it.
#[test]
fn strings_convert_to_wtf8() {
    Harness::new().run_in_context(|lock, ctx| {
        let value = ctx.eval_raw(lock, "'a\\ud800b\\u00e9'").unwrap();
        let string = v8::Local::<v8::String>::try_from(value).unwrap();
        assert_eq!(wtf8(lock, string), b"a\xed\xa0\x80b\xc3\xa9");
        Ok(())
    });
}

#[test]
fn exceptions_are_described_by_their_stack() {
    let description = describe("(() => { const e = new TypeError('boom'); return e; })()");
    assert!(
        description.starts_with(b"TypeError: boom\n"),
        "{description:?}"
    );
    assert_eq!(describe("({ stack: 'custom stack' })"), b"custom stack");
}

#[test]
fn exceptions_without_a_stack_are_converted_to_strings() {
    assert_eq!(describe("'thrown string'"), b"thrown string");
    assert_eq!(describe("42"), b"42");
    assert_eq!(
        describe("({ stack: 1, toString() { return 'object' } })"),
        b"object"
    );
}

#[test]
fn exceptions_that_cannot_be_converted_have_a_placeholder() {
    let placeholder = b"(an exception that can't be converted to a string)";
    assert_eq!(describe("Symbol('s')"), placeholder);
    assert_eq!(
        describe("({ get stack() { throw 1; } })"),
        placeholder.as_slice()
    );
    assert_eq!(
        describe("({ toString() { throw new Error('no'); } })"),
        placeholder.as_slice()
    );
}
