// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An `Error` of a `DOMException` type is thrown as a `DOMException`, and
//! `Error::new_dom_exception` and `Lock::dom_exception` create one with any name. Creating one
//! while an exception or a termination is pending is tested in `pending_exception.rs` and
//! `termination.rs`.
//!
//! A `DOMException` that Rust JSG creates is an instance of the C++ class, or, when the per-isolate
//! bootstrap implements `DOMException` in TypeScript, of the bootstrap's class, which
//! [`set_up_bootstrap_dom_exception`] stands in for.

use jsg::v8;
use jsg_macros::jsg_method;
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

/// Stands in for the exports of the per-isolate bootstrap's `dom-exception` module when it
/// implements `DOMException` in TypeScript. Native code recognizes the class by the private symbol
/// on its prototype, and gives each instance it creates its name and message under the private
/// symbols that the getters read. Only native code creates instances, so the constructor is never
/// called.
const BOOTSTRAP_DOM_EXCEPTION: &str = r"(() => {
  class DOMException {
    get name() { return this[kName]; }
    get message() { return this[kMessage]; }
  }
  Object.setPrototypeOf(DOMException.prototype, Error.prototype);
  DOMException.prototype[kPrototype] = true;
  globalThis.DOMException = DOMException;
  return { DOMException };
})()";

/// Makes [`BOOTSTRAP_DOM_EXCEPTION`] the bootstrap's `dom-exception` module, and its class the
/// global `DOMException`, so that the `DOMException`s native code creates in the context are
/// instances of it.
pub(super) fn set_up_bootstrap_dom_exception(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>) {
    for (global, name) in [
        ("kPrototype", "workerd:domExceptionPrototype"),
        ("kName", "workerd:errorName"),
        ("kMessage", "workerd:errorMessage"),
    ] {
        let symbol = Harness::private_symbol(lock, name);
        ctx.set_global(lock, global, symbol);
    }
    let exports = ctx
        .eval_raw(lock, BOOTSTRAP_DOM_EXCEPTION)
        .unwrap_or_else(|_| panic!("defining the bootstrap's DOMException should not throw"));
    ctx.set_bootstrap_module(lock, "dom-exception", exports);
    let exception = lock.dom_exception("Error", "").unwrap();
    ctx.set_global(lock, "created", exception);
    assert!(
        ctx.eval::<bool>(
            lock,
            "Object.getPrototypeOf(created) === DOMException.prototype"
        )
        .unwrap(),
        "Rust JSG should create the bootstrap's DOMException"
    );
}

/// The class of the `DOMException`s that native code creates.
#[derive(Clone, Copy, Debug)]
enum DomExceptionClass {
    /// C++ JSG's, which native code creates when the context has no bootstrap.
    Cpp,
    /// The bootstrap's, which [`set_up_bootstrap_dom_exception`] stands in for.
    Bootstrap,
}

const DOM_EXCEPTION_CLASSES: [DomExceptionClass; 2] =
    [DomExceptionClass::Cpp, DomExceptionClass::Bootstrap];

/// Runs `callback` in a context of `harness` with `Errors` as a global, in which native code
/// creates `DOMException`s of `class`.
fn run<F>(harness: &Harness, class: DomExceptionClass, callback: F)
where
    F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> jsg::Result<()>,
{
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Errors>(lock)?;
        ctx.set_global(lock, "Errors", constructor);
        if matches!(class, DomExceptionClass::Bootstrap) {
            set_up_bootstrap_dom_exception(lock, ctx);
        }
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

/// With the bootstrap's `DOMException`, each way Rust JSG creates a `DOMException` creates an
/// instance of the bootstrap's class, with the name and message it is given.
#[test]
fn creates_the_bootstrap_dom_exception() {
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
                DomExceptionClass::Bootstrap,
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
