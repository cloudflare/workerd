// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An `Error` of a `DOMException` type is thrown as a `DOMException`, and
//! `Error::new_dom_exception` and `Lock::dom_exception` create one with any name. Creating one while an exception or a termination is pending is
//! tested in `pending_exception.rs` and `termination.rs`.

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Errors;

#[jsg_resource]
impl Errors {
    /// Fails with an `Error` of the type `name` names.
    #[jsg_method]
    fn fail(name: String, message: String) -> jsg::Result<()> {
        Err(jsg::Error::new(&name, &message))
    }

    /// Fails with a `DOMException` named `name`.
    #[jsg_method]
    fn fail_dom(name: String, message: String) -> jsg::Result<()> {
        Err(jsg::Error::new_dom_exception(name, message))
    }

    /// Fails with `error`, read as an `Error`.
    #[jsg_method]
    fn rethrow(error: jsg::Error) -> jsg::Result<()> {
        Err(error)
    }

    /// Returns the exception that `Lock::exception` creates for an `Error` of the type `name`
    /// names, as code that rejects a promise with it does.
    #[jsg_method]
    fn exception<'s>(
        lock: &mut jsg::Lock<'s>,
        name: String,
        message: String,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        lock.exception(&jsg::Error::new(&name, &message))
    }

    /// Returns a `DOMException` named `name`.
    #[jsg_method]
    fn dom_exception<'s>(
        lock: &mut jsg::Lock<'s>,
        name: String,
        message: String,
    ) -> jsg::Result<v8::Local<'s, v8::Object>> {
        lock.dom_exception(&name, &message)
    }
}

/// The `DOMException` names of `ExceptionType`, with their legacy codes.
const DOM_EXCEPTION_TYPES: &[(&str, u32)] = &[
    ("OperationError", 0),
    ("DataError", 0),
    ("DataCloneError", 25),
    ("InvalidAccessError", 15),
    ("InvalidStateError", 11),
    ("InvalidCharacterError", 5),
    ("NotSupportedError", 9),
    ("TimeoutError", 23),
    ("TypeMismatchError", 17),
    ("AbortError", 20),
    ("NotFoundError", 8),
];

/// Evaluates `code` with `Errors` as a global and returns the string it produces.
fn eval(code: &str) -> String {
    let mut result = None;
    crate::Harness::new().run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Errors>(lock)?;
        ctx.set_global(lock, "Errors", constructor);
        result = Some(match ctx.eval::<String>(lock, code) {
            Ok(result) => result,
            Err(err) => panic!("{code}: {}", err.unwrap_jsg_err(lock)),
        });
        Ok(())
    });
    result.unwrap()
}

/// Describes the value that `expr` throws or evaluates to: whether it is an `Error`, and its class,
/// name, message and code. (The harness's global has no `DOMException` property.)
fn describe(expr: &str) -> String {
    eval(&format!(
        "let e; try {{ e = {expr}; }} catch (thrown) {{ e = thrown; }}
         `${{e instanceof Error}} ${{e.constructor.name}} ${{e.name}}: ${{e.message}} \
          (${{e.code}})`"
    ))
}

#[test]
fn dom_exception_types_are_thrown_as_dom_exceptions() {
    for &(name, code) in DOM_EXCEPTION_TYPES {
        assert!(jsg::ExceptionType::from(name).is_dom_exception(), "{name}");
        assert_eq!(
            describe(&format!("Errors.fail('{name}', 'went wrong')")),
            format!("true DOMException {name}: went wrong ({code})"),
        );
    }
}

#[test]
fn javascript_error_types_are_thrown_as_their_class() {
    for name in [
        "Error",
        "TypeError",
        "RangeError",
        "ReferenceError",
        "SyntaxError",
    ] {
        assert!(!jsg::ExceptionType::from(name).is_dom_exception(), "{name}");
        assert_eq!(
            describe(&format!("Errors.fail('{name}', 'went wrong')")),
            format!("true {name} {name}: went wrong (undefined)"),
        );
    }
}

#[test]
fn exception_creates_a_dom_exception() {
    assert_eq!(
        describe("Errors.exception('AbortError', 'aborted')"),
        "true DOMException AbortError: aborted (20)",
    );
    assert_eq!(
        describe("Errors.exception('TypeError', 'bad')"),
        "true TypeError TypeError: bad (undefined)",
    );
}

#[test]
fn dom_exception_accepts_any_name() {
    assert_eq!(
        describe("Errors.domException('IndexSizeError', 'too big')"),
        "true DOMException IndexSizeError: too big (1)",
    );
    assert_eq!(
        describe("Errors.domException('SyntaxError', 'bad syntax')"),
        "true DOMException SyntaxError: bad syntax (12)",
    );
    assert_eq!(
        describe("Errors.domException('MadeUpError', '')"),
        "true DOMException MadeUpError:  (0)",
    );
}

#[test]
fn dom_exception_has_a_stack() {
    assert_eq!(
        eval(
            "let e; try { Errors.fail('AbortError', 'aborted'); } catch (thrown) { e = thrown; }
             String(typeof e.stack === 'string' && e.stack.startsWith('AbortError: aborted'))"
        ),
        "true",
    );
}

#[test]
fn new_dom_exception_accepts_any_name() {
    assert_eq!(
        describe("Errors.failDom('QuotaExceededError', 'full')"),
        "true DOMException QuotaExceededError: full (22)",
    );
    assert_eq!(
        describe("Errors.failDom('SyntaxError', 'bad syntax')"),
        "true DOMException SyntaxError: bad syntax (12)",
    );
    assert_eq!(
        describe("Errors.failDom('DataError', 'bad data')"),
        describe("Errors.fail('DataError', 'bad data')"),
    );
}

#[test]
fn javascript_error_named_like_a_dom_exception_stays_a_class() {
    assert_eq!(
        describe("Errors.rethrow(Object.assign(new Error('x'), { name: 'DataError' }))"),
        "true Error Error: x (undefined)",
    );
    assert_eq!(
        describe("Errors.rethrow(new SyntaxError('s'))"),
        "true SyntaxError SyntaxError: s (undefined)",
    );
}
