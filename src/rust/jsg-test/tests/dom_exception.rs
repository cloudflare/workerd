// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An `Error` of a `DOMException` type is thrown as a `DOMException`, and
//! `Error::new_dom_exception` and `Lock::dom_exception` create one with any name. `FromJS` for
//! `Error` reads a `DOMException` back as one. Creating one while an exception or a termination
//! is pending is tested in `pending_exception.rs` and `termination.rs`.
//!
//! A `DOMException` that Rust JSG creates is an instance of the C++ class, or, under the
//! `V8_DOM_EXCEPTION` autogate, of V8's.

use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

use super::exception_to_kj::assert_same_value;
use crate::EvalContext;
use crate::Harness;

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

    /// Converts `exception` to a `kj::Exception` and back with `jsg::TunneledException`.
    #[jsg_method]
    fn tunnel<'s>(
        lock: &mut jsg::Lock<'s>,
        exception: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        jsg::TunneledException::from_js(lock, exception)?.to_js(lock)
    }

    /// The name and message that `jsg::try_unwrap_dom_exception` reads from `value`, as
    /// `name: message`, or `undefined` if `value` is not a `DOMException`.
    #[jsg_method]
    fn try_unwrap<'s>(
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<Option<String>> {
        Ok(jsg::try_unwrap_dom_exception(lock, value)?
            .map(|(name, message)| format!("{name}: {message}")))
    }
}

/// A resource whose constructor fails with a `DOMException`.
#[jsg_resource]
struct FailingConstructor;

#[jsg_resource]
impl FailingConstructor {
    #[jsg_constructor]
    fn constructor() -> jsg::Result<Self> {
        Err(jsg::Error::new_not_supported_error("constructor"))
    }
}

/// A resource whose getter fails with a `DOMException`.
#[jsg_resource]
struct FailingGetter;

#[jsg_resource]
impl FailingGetter {
    #[jsg_property(prototype, readonly)]
    fn get_state(&self) -> jsg::Result<f64> {
        Err(jsg::Error::new_invalid_state_error("getter"))
    }
}

/// The class of the `DOMException`s that native code creates.
#[derive(Clone, Copy, Debug)]
pub(super) enum DomExceptionClass {
    /// C++ JSG's, which native code creates without the `V8_DOM_EXCEPTION` autogate.
    Cpp,
    /// V8's, which native code creates under the autogate.
    V8,
}

const DOM_EXCEPTION_CLASSES: [DomExceptionClass; 2] =
    [DomExceptionClass::Cpp, DomExceptionClass::V8];

/// Configures the `V8_DOM_EXCEPTION` autogate for `class`, for as long as the guard lives. A
/// context created meanwhile has the `DOMException` global of `class`.
pub(super) fn set_autogates_for(class: DomExceptionClass) -> crate::Autogates {
    crate::set_autogates(match class {
        DomExceptionClass::Cpp => &[],
        DomExceptionClass::V8 => &["v8-dom-exception"],
    })
}

/// Runs `callback` in a context of `harness` with `Errors` as a global, in which native code
/// creates `DOMException`s of `class`.
fn run<F>(harness: &Harness, class: DomExceptionClass, callback: F)
where
    F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> jsg::Result<()>,
{
    let _autogates = set_autogates_for(class);
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Errors>(lock)?;
        ctx.set_global(lock, "Errors", constructor);
        callback(lock, ctx)
    });
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
    eval_with(DomExceptionClass::Cpp, code)
}

/// Evaluates `code` as [`eval`] does, in a context in which native code creates `DOMException`s
/// of `class`.
fn eval_with(class: DomExceptionClass, code: &str) -> String {
    let mut result = None;
    run(&Harness::new(), class, |lock, ctx| {
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

/// Under the `V8_DOM_EXCEPTION` autogate, each way Rust JSG creates a `DOMException` creates an
/// instance of V8's, with the name and message it is given.
#[test]
fn creates_v8s_dom_exception() {
    for (expr, expected) in [
        // An `Error` of a `DOMException` type, as `jsg_require!` returns one.
        (
            "Errors.fail('AbortError', 'aborted')",
            "AbortError: aborted",
        ),
        (
            "Errors.failDom('QuotaExceededError', 'full')",
            "QuotaExceededError: full",
        ),
        (
            "Errors.exception('DataError', 'bad data')",
            "DataError: bad data",
        ),
        ("Errors.domException('MadeUpError', '')", "MadeUpError: "),
        (
            "Errors.tunnel(Errors.domException('TimeoutError', 'late'))",
            "TimeoutError: late",
        ),
    ] {
        assert_eq!(
            eval_with(
                DomExceptionClass::V8,
                &format!(
                    "let e; try {{ e = {expr}; }} catch (thrown) {{ e = thrown; }}
                     `${{Object.getPrototypeOf(e) === DOMException.prototype}} \
                      ${{e.name}}: ${{e.message}}`"
                ),
            ),
            format!("true {expected}"),
            "{expr}",
        );
    }
}

/// `jsg::try_unwrap_dom_exception` reads the name and message a `DOMException` was created with,
/// which own properties don't change, from an instance of either class alike.
#[test]
fn try_unwrap_dom_exception_reads_both_classes_alike() {
    let cases = [
        (
            "Errors.domException('DataError', 'boom')",
            "DataError: boom",
        ),
        (
            "Errors.domException('Data\\0Error', 'a\\0b')",
            "Data\0Error: a\0b",
        ),
        (
            "Errors.tunnel(Errors.domException('AbortError', 'aborted'))",
            "AbortError: aborted",
        ),
        (
            "Object.defineProperty(Errors.domException('DataError', 'boom'), 'name', \
             { value: 'Other' })",
            "DataError: boom",
        ),
        (
            "Object.defineProperty(Errors.domException('DataError', 'boom'), 'message', \
             { value: 'other' })",
            "DataError: boom",
        ),
        (
            "Object.assign(new Error('boom'), { name: 'DataError' })",
            "none",
        ),
    ];
    for class in DOM_EXCEPTION_CLASSES {
        run(&Harness::new(), class, |lock, ctx| {
            for (expr, expected) in cases {
                let read: String = ctx
                    .eval(lock, &format!("Errors.tryUnwrap({expr}) ?? 'none'"))
                    .unwrap();
                assert_eq!(read, expected, "{expr}, {class:?}");
            }
            Ok(())
        });
    }
}

/// `Lock::exception_to_kj` converts the `DOMException`s Rust JSG creates as C++ JSG's
/// `jsg::Lock::exceptionToKj()` converts them, and describes an instance of either class alike.
#[test]
fn exception_to_kj_matches_cpp_jsg_for_both_classes() {
    let values = [
        "Errors.domException('AbortError', 'boom')",
        "Errors.domException('Error', '')",
        "Errors.domException('Data\\0Error', 'a\\0b')",
        "Errors.tunnel(Errors.domException('DataError', 'boom'))",
        "Object.assign(Errors.domException('DataError', 'boom'), { overloaded: true })",
        "Object.assign(Errors.domException('DataError', 'boom'), { retryable: true })",
        "Object.defineProperty(Errors.domException('DataError', 'boom'), 'name', { value: 'Other' })",
        "Object.defineProperty(Errors.domException('DataError', 'boom'), 'message', \
         { value: 'other' })",
    ];
    for enhanced in [false, true] {
        let descriptions = DOM_EXCEPTION_CLASSES.map(|class| {
            let harness =
                Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(enhanced));
            let mut descriptions = Vec::new();
            run(&harness, class, |lock, ctx| {
                for expr in values {
                    let value = ctx
                        .eval_raw(lock, expr)
                        .unwrap_or_else(|_| panic!("{expr} should evaluate"));
                    let what =
                        format!("{expr}, {class:?}, enhanced_error_serialization: {enhanced}");
                    let exception = assert_same_value(lock, value, &what);
                    descriptions.push(exception.description().to_owned());
                }
                Ok(())
            });
            descriptions
        });
        assert_eq!(
            descriptions[0], descriptions[1],
            "enhanced_error_serialization: {enhanced}"
        );
    }
}

/// A constructor and a getter that fail with a `DOMException` throw it, as a method does.
#[test]
fn constructors_and_getters_throw_dom_exceptions() {
    run(&Harness::new(), DomExceptionClass::Cpp, |lock, ctx| {
        let constructor = jsg::resource::function_template_of::<FailingConstructor>(lock)?;
        ctx.set_global(lock, "FailingConstructor", constructor);
        let failing = jsg::Rc::new(lock, FailingGetter);
        ctx.set_global(lock, "failing", failing);
        for (expr, expected) in [
            (
                "new FailingConstructor()",
                "true DOMException NotSupportedError: constructor (9)",
            ),
            (
                "failing.state",
                "true DOMException InvalidStateError: getter (11)",
            ),
        ] {
            let thrown: String = ctx
                .eval(
                    lock,
                    &format!(
                        "(() => {{
                           let e; try {{ {expr}; }} catch (thrown) {{ e = thrown; }}
                           return `${{e instanceof Error}} ${{e.constructor.name}} ${{e.name}}: \
                                   ${{e.message}} (${{e.code}})`;
                         }})()"
                    ),
                )
                .unwrap();
            assert_eq!(thrown, expected, "{expr}");
        }
        Ok(())
    });
}

/// `FromJS` for `Error` reads a `DOMException` of either class as a `DOMException`, with the name
/// and message it was created with, whatever the name, including `SyntaxError` and names that are
/// no `ExceptionType`.
#[test]
fn from_js_reads_a_dom_exception() {
    let names = DOM_EXCEPTION_TYPES.iter().map(|&(name, _)| name).chain([
        "SyntaxError",
        "QuotaExceededError",
        "IndexSizeError",
        "MadeUpError",
    ]);
    for class in DOM_EXCEPTION_CLASSES {
        run(&Harness::new(), class, |lock, ctx| {
            for name in names.clone() {
                let value = ctx
                    .eval_raw(
                        lock,
                        &format!("Errors.domException('{name}', 'went wrong')"),
                    )
                    .unwrap();
                let err = <jsg::Error as jsg::FromJS>::from_js(lock, value).unwrap();
                assert!(err.name.is_dom_exception(), "{name}, {class:?}");
                assert_eq!(err.name.as_str(), name, "{class:?}");
                assert_eq!(err.message, "went wrong", "{name}, {class:?}");
            }

            // The name and message it was created with, not own properties that shadow them.
            let value = ctx
                .eval_raw(
                    lock,
                    "Object.defineProperties(Errors.domException('DataError', 'boom'), {
                       name: { value: 'TypeError' },
                       message: { value: 'other' },
                     })",
                )
                .unwrap();
            let err = <jsg::Error as jsg::FromJS>::from_js(lock, value).unwrap();
            assert!(err.name.is_dom_exception(), "{class:?}");
            assert_eq!(err.name.as_str(), "DataError", "{class:?}");
            assert_eq!(err.message, "boom", "{class:?}");

            // An object that only looks like one is still not an error.
            let value = ctx
                .eval_raw(lock, "({ name: 'AbortError', message: 'm' })")
                .unwrap();
            let err = <jsg::Error as jsg::FromJS>::from_js(lock, value).unwrap_err();
            assert_eq!(err.name, jsg::ExceptionType::TypeError, "{class:?}");
            Ok(())
        });
    }
}

/// A `DOMException` that JavaScript passes to Rust, and Rust throws again, is thrown as the same
/// `DOMException`, with either class.
#[test]
fn rethrown_dom_exceptions_stay_dom_exceptions() {
    for class in DOM_EXCEPTION_CLASSES {
        for (expr, expected) in [
            (
                "(() => { try { Errors.fail('AbortError', 'aborted'); } catch (e) { return e; } })()",
                "AbortError: aborted",
            ),
            (
                "Errors.domException('SyntaxError', 'bad syntax')",
                "SyntaxError: bad syntax",
            ),
            (
                "Errors.domException('IndexSizeError', 'too big')",
                "IndexSizeError: too big",
            ),
        ] {
            assert_eq!(
                eval_with(
                    class,
                    &format!(
                        "const original = {expr};
                         let e; try {{ Errors.rethrow(original); }} catch (thrown) {{ e = thrown; }}
                         `${{e.constructor === original.constructor}} ${{e.name}}: ${{e.message}}`"
                    ),
                ),
                format!("true {expected}"),
                "{expr}, {class:?}",
            );
        }
    }
}

/// A `DOMException` that a script creates with C++ JSG's class, or a subclass of it, converts as
/// one that native code creates does.
#[test]
fn script_created_dom_exceptions_convert() {
    for (expr, expected) in [
        (
            "new DOMException('m', 'AbortError')",
            "true DOMException AbortError: m (20)",
        ),
        (
            "new DOMException('m', 'MadeUpError')",
            "true DOMException MadeUpError: m (0)",
        ),
        (
            "new (class extends DOMException {})('m', 'NotFoundError')",
            "true DOMException NotFoundError: m (8)",
        ),
    ] {
        assert_eq!(
            describe(&format!(
                "(() => {{
                   const DOMException = Errors.domException('Error', '').constructor;
                   return Errors.rethrow({expr});
                 }})()"
            )),
            expected,
            "{expr}",
        );
    }
}
