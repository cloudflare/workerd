// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A pending termination reaches the caller that started the JavaScript: nothing Rust throws
//! after it replaces it, and JavaScript cannot catch it. See `src/rust/jsg/README.md` §Safety
//! rules, rule 1.
//!
//! Each method first has the harness deliver a termination, as a call into JavaScript that ran
//! out of CPU time would, so that the termination exception is pending when the method goes on.

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

thread_local! {
    /// Whether converting the array in `terminate_then_return_dates` failed with a pending error.
    static DATES_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
        const { std::cell::Cell::new(None) };
    /// Whether wrapping the resource in `terminate_then_return_resource` failed with a pending
    /// error.
    static RESOURCE_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
        const { std::cell::Cell::new(None) };
}

#[jsg_resource]
struct Terminator;

#[jsg_resource]
impl Terminator {
    /// Is terminated and returns normally.
    #[jsg_method]
    fn terminate(&self, lock: &mut jsg::Lock) {
        assert!(!lock.is_execution_terminating());
        crate::Harness::terminate_and_deliver(lock);
        assert!(lock.is_execution_terminating());
    }

    /// Is terminated and then fails, as a method does when it propagates the error of a
    /// terminated call into JavaScript with `?`.
    #[jsg_method]
    fn terminate_then_fail(&self, lock: &mut jsg::Lock) -> Result<(), jsg::Error> {
        crate::Harness::terminate_and_deliver(lock);
        Err(jsg::Error::new_type_error("thrown while terminating"))
    }

    #[jsg_method]
    fn terminate_then_throw_error(&self, lock: &mut jsg::Lock) {
        crate::Harness::terminate_and_deliver(lock);
        lock.throw_error("thrown while terminating");
    }

    /// Is terminated and then returns a time, which is converted to a `Date` after the
    /// termination.
    #[jsg_method]
    fn terminate_then_return_date(&self, lock: &mut jsg::Lock) -> std::time::SystemTime {
        crate::Harness::terminate_and_deliver(lock);
        std::time::UNIX_EPOCH
    }

    /// Is terminated and then converts an array of times, whose conversion passes on the
    /// pending error from converting the time.
    #[jsg_method]
    fn terminate_then_return_dates<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::ToJS::to_js(vec![std::time::UNIX_EPOCH], lock);
        DATES_CONVERSION_PENDING.set(Some(result.as_ref().is_err_and(jsg::Error::is_pending)));
        result
    }

    /// Is terminated and then wraps a new resource, which creates its wrapper.
    #[jsg_method]
    fn terminate_then_return_resource<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::ToJS::to_js(jsg::Rc::new(Self), lock);
        RESOURCE_CONVERSION_PENDING.set(Some(result.as_ref().is_err_and(jsg::Error::is_pending)));
        result
    }

    /// Is terminated and then creates and returns a `BigInt`, which V8 creates without
    /// preparing to run JavaScript.
    #[jsg_method]
    fn terminate_then_return_big_int<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::v8::Local<'s, jsg::v8::BigInt> {
        crate::Harness::terminate_and_deliver(lock);
        jsg::v8::BigInt::new(lock, 1)
    }

    #[jsg_method]
    fn terminate_then_throw_internal_error(&self, lock: &mut jsg::Lock) {
        crate::Harness::terminate_and_deliver(lock);
        lock.throw_internal_error("thrown while terminating");
    }
}

/// Calls `method` on a `Terminator` from inside a JavaScript `try`/`catch`, and checks that the
/// termination reaches the harness: the script fails without a catchable exception, and neither
/// the `catch` block nor the code after it runs.
fn termination_survives(method: &str) {
    crate::Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "obj", jsg::Rc::new(Terminator));
        let script = format!(
            "globalThis.caught = 'nothing'; globalThis.after = false;
             try {{ obj.{method}(); }} catch (e) {{ globalThis.caught = String(e); }}
             globalThis.after = true;"
        );
        match ctx.eval_raw(lock, &script) {
            Err(crate::EvalError::EvalFailed) => {}
            Err(crate::EvalError::Exception(exception)) => {
                assert!(
                    exception.is_null(),
                    "{method}: the termination was replaced by a catchable exception"
                );
            }
            other => panic!("{method}: the script was not terminated: {other:?}"),
        }
        // V8 clears a termination once the last JavaScript frame has unwound, so the isolate
        // runs JavaScript again.
        assert!(!lock.is_execution_terminating());
        assert_eq!(
            ctx.eval::<String>(lock, "globalThis.caught").unwrap(),
            "nothing",
            "{method}: JavaScript caught the termination"
        );
        assert!(
            !ctx.eval::<bool>(lock, "globalThis.after").unwrap(),
            "{method}: JavaScript ran on after the termination"
        );
        Ok(())
    });
}

#[test]
fn termination_is_not_catchable() {
    termination_survives("terminate");
}

#[test]
fn failing_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenFail");
}

#[test]
fn throw_error_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenThrowError");
}

#[test]
fn throw_internal_error_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenThrowInternalError");
}

#[test]
fn returning_a_date_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenReturnDate");
}

#[test]
fn returning_dates_while_terminating_keeps_the_termination() {
    DATES_CONVERSION_PENDING.set(None);
    termination_survives("terminateThenReturnDates");
    assert_eq!(DATES_CONVERSION_PENDING.get(), Some(true));
}

#[test]
fn returning_a_resource_while_terminating_keeps_the_termination() {
    RESOURCE_CONVERSION_PENDING.set(None);
    termination_survives("terminateThenReturnResource");
    assert_eq!(RESOURCE_CONVERSION_PENDING.get(), Some(true));
}

#[test]
fn returning_a_big_int_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenReturnBigInt");
}
