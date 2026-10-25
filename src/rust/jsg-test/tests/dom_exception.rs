// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The DOM-prefixed `ExceptionType`s throw a `DOMException`, as the C++
//! `JSG_REQUIRE(..., DOMSyntaxError, ...)` error types do, and `FromJS` for
//! `jsg::Error` recognizes a `DOMException`.

use jsg::ExceptionType;
use jsg::Number;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

/// Each DOM-prefixed type with the name and legacy `code` the thrown
/// `DOMException` has (`src/workerd/jsg/dom-exception.h`).
#[rustfmt::skip]
const DOM_TYPES: [(ExceptionType, &str, u16); 13] = [
    (
        ExceptionType::DOMOperationError,
        "OperationError",
        0,
    ),
    (
        ExceptionType::DOMDataError,
        "DataError",
        0,
    ),
    (
        ExceptionType::DOMDataCloneError,
        "DataCloneError",
        25,
    ),
    (
        ExceptionType::DOMInvalidAccessError,
        "InvalidAccessError",
        15,
    ),
    (
        ExceptionType::DOMInvalidStateError,
        "InvalidStateError",
        11,
    ),
    (
        ExceptionType::DOMInvalidCharacterError,
        "InvalidCharacterError",
        5,
    ),
    (
        ExceptionType::DOMNotSupportedError,
        "NotSupportedError",
        9,
    ),
    (
        ExceptionType::DOMSyntaxError,
        "SyntaxError",
        12,
    ),
    (
        ExceptionType::DOMTimeoutError,
        "TimeoutError",
        23,
    ),
    (
        ExceptionType::DOMTypeMismatchError,
        "TypeMismatchError",
        17,
    ),
    (
        ExceptionType::DOMQuotaExceededError,
        "QuotaExceededError",
        22,
    ),
    (
        ExceptionType::DOMAbortError,
        "AbortError",
        20,
    ),
    (
        ExceptionType::DOMNotFoundError,
        "NotFoundError",
        8,
    ),
];

/// The JS error classes, which must stay built-in errors.
const JS_TYPES: [(ExceptionType, &str); 5] = [
    (ExceptionType::Error, "Error"),
    (ExceptionType::TypeError, "TypeError"),
    (ExceptionType::RangeError, "RangeError"),
    (ExceptionType::ReferenceError, "ReferenceError"),
    (ExceptionType::SyntaxError, "SyntaxError"),
];

#[jsg_resource]
struct Thrower;

#[jsg_resource]
impl Thrower {
    /// Fails with the DOM type at `index` in `DOM_TYPES`, or for a negative
    /// `index`, the JS type at `-index - 1` in `JS_TYPES`.
    #[jsg_method]
    pub fn fail(&self, index: i32) -> Result<Number, jsg::Error> {
        let ty = match usize::try_from(index) {
            Ok(i) => DOM_TYPES[i].0,
            Err(_) => JS_TYPES[usize::try_from(-index - 1).unwrap()].0,
        };
        Err(jsg::Error::from_type(ty, format!("failed {index}")))
    }

    /// Fails with a `DOMException` named `name`.
    #[jsg_method]
    pub fn fail_named(&self, name: String) -> Result<Number, jsg::Error> {
        Err(jsg::Error::new_dom_exception(&name, "failed"))
    }

    /// Converts `value` to a `jsg::Error` and fails with it.
    #[jsg_method]
    pub fn rethrow<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: jsg::v8::Local<'s, jsg::v8::Value>,
    ) -> Result<Number, jsg::Error> {
        Err(<jsg::Error as jsg::FromJS>::from_js(lock, value)?)
    }

    /// Fails with the error a tunneled KJ exception with `description` converts to.
    #[jsg_method]
    pub fn fail_tunneled(&self, description: String) -> Result<Number, jsg::Error> {
        Err(crate::ffi::throw_kj_exception(&description)
            .unwrap_err()
            .into())
    }

    #[jsg_property(readonly)]
    pub fn get_failing(&self) -> Result<Number, jsg::Error> {
        Err(jsg::Error::new_dom_invalid_state_error("getter"))
    }
}

/// A resource whose constructor fails with a `DOMException`.
#[jsg_resource]
struct FailingConstructor;

#[jsg_resource]
impl FailingConstructor {
    #[jsg_constructor]
    fn constructor() -> jsg::Result<Self> {
        Err(jsg::Error::new_dom_not_supported_error("constructor"))
    }
}

fn set_thrower(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>) {
    ctx.set_global(lock, "thrower", jsg::Rc::new(Thrower));
}

/// Describes what `call` throws as `constructor name/name/message/code/is Error`.
fn describe_thrown(call: &str) -> String {
    format!(
        "(() => {{
           try {{ {call}; return 'did not throw'; }}
           catch (e) {{
             return [e.constructor.name, e.name, e.message, e.code, e instanceof Error].join('/');
           }}
         }})()"
    )
}

#[test]
fn dom_types_throw_dom_exceptions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        for (index, (_, name, code)) in DOM_TYPES.iter().enumerate() {
            let thrown: String = ctx
                .eval(lock, &describe_thrown(&format!("thrower.fail({index})")))
                .unwrap();
            assert_eq!(
                thrown,
                format!("DOMException/{name}/failed {index}/{code}/true")
            );
        }
        Ok(())
    });
}

#[test]
fn js_types_throw_built_in_errors() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        for (i, (_, name)) in JS_TYPES.iter().enumerate() {
            let index = -i32::try_from(i).unwrap() - 1;
            let call = format!("thrower.fail({index})");
            let thrown: String = ctx.eval(lock, &describe_thrown(&call)).unwrap();
            assert_eq!(thrown, format!("{name}/{name}/failed {index}//true"));
            let is_built_in: bool = ctx
                .eval(
                    lock,
                    &format!("(() => {{ try {{ {call} }} catch (e) {{ return e instanceof {name}; }} }})()"),
                )
                .unwrap();
            assert!(is_built_in, "{name}");
        }
        Ok(())
    });
}

#[test]
fn thrown_dom_exceptions_convert_back_to_their_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        for (index, (ty, _, _)) in DOM_TYPES.iter().enumerate() {
            let err = ctx
                .eval::<Number>(lock, &format!("thrower.fail({index})"))
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(err.name, *ty);
            assert_eq!(err.message, format!("failed {index}"));
        }
        let err = ctx
            .eval::<Number>(lock, "thrower.fail(-5)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::SyntaxError);
        Ok(())
    });
}

#[test]
fn script_created_dom_exceptions_convert() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        ctx.eval_raw(
            lock,
            "globalThis.DOMException = (() => { try { thrower.fail(0) } catch (e) { return e.constructor; } })()",
        )
        .unwrap();

        let convert = |lock: &mut jsg::Lock, expr: &str| {
            let value = ctx.eval_raw(lock, expr).unwrap();
            <jsg::Error as jsg::FromJS>::from_js(lock, value).unwrap()
        };

        let err = convert(lock, "new DOMException('m', 'AbortError')");
        assert_eq!(err.name, ExceptionType::DOMAbortError);
        assert_eq!(err.message, "m");

        // A name without a DOM-prefixed type, legacy or chosen by the script.
        for name in ["IndexSizeError", "CustomError"] {
            let err = convert(lock, &format!("new DOMException('m', '{name}')"));
            assert_eq!(err.name, ExceptionType::Error, "{name}");
            assert_eq!(err.message, "m", "{name}");
        }

        // The name and message C++ stores, not the configurable JS properties.
        let err = convert(
            lock,
            "(() => {
               const e = new DOMException('m', 'DataError');
               Object.defineProperty(e, 'name', { value: 'TypeError' });
               Object.defineProperty(e, 'message', { value: 'overridden' });
               if (e.name !== 'TypeError' || e.message !== 'overridden') throw 'not overridden';
               return e;
             })()",
        );
        assert_eq!(err.name, ExceptionType::DOMDataError);
        assert_eq!(err.message, "m");

        // A subclass instance has a DOMException on its prototype chain.
        let err = convert(
            lock,
            "(() => { class Sub extends DOMException {} return new Sub('m', 'NotFoundError'); })()",
        );
        assert_eq!(err.name, ExceptionType::DOMNotFoundError);

        // Plain objects are still not errors.
        let value = ctx.eval_raw(lock, "({ name: 'AbortError', message: 'm' })").unwrap();
        let err = <jsg::Error as jsg::FromJS>::from_js(lock, value).unwrap_err();
        assert_eq!(err.name, ExceptionType::TypeError);
        Ok(())
    });
}

#[test]
fn constructors_and_properties_throw_dom_exceptions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        let constructor = jsg::resource::function_template_of::<FailingConstructor>(lock);
        ctx.set_global(lock, "FailingConstructor", constructor);
        let thrown: String = ctx
            .eval(lock, &describe_thrown("new FailingConstructor()"))
            .unwrap();
        assert_eq!(thrown, "DOMException/NotSupportedError/constructor/9/true");
        let thrown: String = ctx.eval(lock, &describe_thrown("thrower.failing")).unwrap();
        assert_eq!(thrown, "DOMException/InvalidStateError/getter/11/true");
        Ok(())
    });
}

/// A `DOMException` name without a DOM-prefixed type keeps its name, both when created in Rust
/// and when converted from JavaScript, as C++ JSG keeps it.
#[test]
fn dom_exception_names_without_a_type_are_kept() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        for name in ["IndexSizeError", "CustomError"] {
            let err = jsg::Error::new_dom_exception(name, "m");
            assert_eq!(err.name, ExceptionType::Error);
            assert_eq!(err.dom_exception_name(), Some(name));
            assert_eq!(err.to_string(), format!("DOMException({name}): m"));

            let thrown: String = ctx
                .eval(lock, &describe_thrown(&format!("thrower.failNamed('{name}')")))
                .unwrap();
            let code = if name == "IndexSizeError" { "1" } else { "0" };
            assert_eq!(thrown, format!("DOMException/{name}/failed/{code}/true"));

            let thrown: String = ctx
                .eval(
                    lock,
                    &describe_thrown(&format!(
                        "thrower.rethrow((() => {{ try {{ thrower.failNamed('{name}') }} catch (e) {{ return e; }} }})())"
                    )),
                )
                .unwrap();
            assert_eq!(thrown, format!("DOMException/{name}/failed/{code}/true"));
        }
        // A name with a type gets the type.
        let err = jsg::Error::new_dom_exception("AbortError", "m");
        assert_eq!(err.name, ExceptionType::DOMAbortError);
        assert_eq!(err.dom_exception_name(), Some("AbortError"));
        assert_eq!(jsg::Error::new_type_error("m").dom_exception_name(), None);
        Ok(())
    });
}

/// A tunneled `jsg.DOMException` is thrown as that `DOMException`, and an internal one as a
/// `DOMException` with the redacted message, as C++ throws it.
#[test]
fn tunneled_dom_exceptions_throw_dom_exceptions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_thrower(lock, ctx);
        let thrown: String = ctx
            .eval(
                lock,
                &describe_thrown("thrower.failTunneled('jsg.DOMException(IndexSizeError): boom')"),
            )
            .unwrap();
        assert_eq!(thrown, "DOMException/IndexSizeError/boom/1/true");

        let thrown: String = ctx
            .eval(
                lock,
                &describe_thrown(
                    "thrower.failTunneled('jsg-internal.DOMException(OperationError): secret')",
                ),
            )
            .unwrap();
        assert_eq!(
            thrown,
            "DOMException/OperationError/internal error; reference = 0123456789abcdefghijklmn/0/true"
        );
        Ok(())
    });
}
