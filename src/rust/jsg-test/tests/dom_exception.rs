// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The DOM-prefixed `ExceptionType`s throw a `DOMException`, as the C++
//! `JSG_REQUIRE(..., DOMSyntaxError, ...)` error types do, and `FromJS` for
//! `jsg::Error` recognizes a `DOMException`.

use jsg::ExceptionType;
use jsg::Number;
use jsg_macros::jsg_method;
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
