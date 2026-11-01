// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A pending termination reaches the caller that started the JavaScript: nothing Rust throws
//! after it replaces it, and JavaScript cannot catch it. See `src/rust/jsg/README.md` §Safety
//! rules, rule 1.
//!
//! Most methods first have the harness deliver a termination, as a call into JavaScript that ran
//! out of CPU time would, so that the termination exception is pending when the method goes on.
//! The `terminate_now` methods terminate with `Lock::terminate_execution_now()` instead.

use std::cell::Cell;

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct Point {
    pub x: u32,
}

thread_local! {
    /// Whether converting the struct in `terminate_then_return_struct` failed with a pending
    /// error.
    static STRUCT_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
        const { std::cell::Cell::new(None) };
    /// Whether converting the array in `terminate_then_return_dates` failed with a pending error.
    static DATES_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
        const { std::cell::Cell::new(None) };
    /// Whether wrapping the resource in `terminate_then_return_resource` failed with a pending
    /// error.
    static RESOURCE_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
        const { std::cell::Cell::new(None) };
    /// Whether wrapping the resource in `terminate_then_return_this` failed with a pending error.
    static THIS_CONVERSION_PENDING: std::cell::Cell<Option<bool>> =
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

    /// Is terminated and then converts a `#[jsg_struct]`.
    #[jsg_method]
    fn terminate_then_return_struct<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::ToJS::to_js(Point { x: 1 }, lock);
        STRUCT_CONVERSION_PENDING.set(Some(result.as_ref().is_err_and(jsg::Error::is_pending)));
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

    /// Is terminated and then wraps its own resource, whose wrapper already exists, so that
    /// wrapping it only reads the wrapper.
    #[jsg_method]
    fn terminate_then_return_this<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::ToJS::to_js(this, lock);
        THIS_CONVERSION_PENDING.set(Some(result.as_ref().is_err_and(jsg::Error::is_pending)));
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

    /// Terminates execution itself, with no termination delivered first, and returns the error,
    /// as `ctx.abort()` does.
    #[jsg_method]
    fn terminate_now<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
        assert!(!lock.is_execution_terminating());
        let err = lock.terminate_execution_now();
        assert!(lock.is_execution_terminating());
        assert!(err.is_pending());
        TERMINATE_NOW_RETURNED.set(true);
        Err(err)
    }

    /// Terminates execution itself, then goes on to throw, which throws nothing.
    #[jsg_method]
    fn terminate_now_then_throw_error(&self, lock: &mut jsg::Lock) {
        let _ = lock.terminate_execution_now();
        lock.throw_error("thrown while terminating");
    }

    /// Terminates execution itself twice, as Rust can after catching a termination. The second
    /// call finds the termination pending and does not enter V8 again.
    #[jsg_method]
    fn terminate_now_twice(&self, lock: &mut jsg::Lock) -> jsg::Result<()> {
        let _ = lock.terminate_execution_now();
        let err = lock.terminate_execution_now();
        assert!(lock.is_execution_terminating());
        assert!(err.is_pending());
        Err(err)
    }

    /// Throws an ordinary exception, leaving it pending, then terminates execution itself. The
    /// termination replaces the exception, so JavaScript cannot catch it.
    #[jsg_method]
    fn throw_error_then_terminate_now(&self, lock: &mut jsg::Lock) -> jsg::Result<()> {
        lock.throw_error("thrown before terminating");
        let err = lock.terminate_execution_now();
        assert!(lock.is_execution_terminating());
        assert!(err.is_pending());
        Err(err)
    }
}

thread_local! {
    /// Whether `terminate_now` returned.
    static TERMINATE_NOW_RETURNED: Cell<bool> = const { Cell::new(false) };
}

/// Calls `method` on a `Terminator` from inside a JavaScript `try`/`catch`, and checks that the
/// termination reaches the harness: the script fails without a catchable exception, and neither
/// the `catch` block nor the code after it runs.
fn termination_survives(method: &str) {
    termination_survives_in(&crate::Harness::new(), method);
}

/// [`termination_survives`] in `harness`.
fn termination_survives_in(harness: &crate::Harness, method: &str) {
    termination_survives_call(
        harness,
        jsg::Rc::new(Terminator),
        &format!("{method}()"),
        |_, _| {},
    );
}

/// Evaluates `obj.{call}` in `harness` from inside a JavaScript `try`/`catch`, and checks that the
/// termination reaches the harness: the script fails without a catchable exception, and neither
/// the `catch` block nor the code after it runs. Then runs `check`, once V8 has cleared the
/// termination.
fn termination_survives_call<R: jsg::Resource + 'static>(
    harness: &crate::Harness,
    obj: jsg::Rc<R>,
    call: &str,
    check: impl FnOnce(&mut jsg::Lock<'_>, &crate::EvalContext<'_>),
) {
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(lock, "obj", obj);
        let script = format!(
            "globalThis.caught = 'nothing'; globalThis.after = false;
             try {{ obj.{call}; }} catch (e) {{ globalThis.caught = String(e); }}
             globalThis.after = true;"
        );
        match ctx.eval_raw(lock, &script) {
            Err(crate::EvalError::EvalFailed) => {}
            Err(crate::EvalError::Exception(exception)) => {
                assert!(
                    exception.is_null(),
                    "{call}: the termination was replaced by a catchable exception"
                );
            }
            other => panic!("{call}: the script was not terminated: {other:?}"),
        }
        // V8 clears a termination once the last JavaScript frame has unwound, so the isolate
        // runs JavaScript again.
        assert!(!lock.is_execution_terminating());
        assert_eq!(
            ctx.eval::<String>(lock, "globalThis.caught").unwrap(),
            "nothing",
            "{call}: JavaScript caught the termination"
        );
        assert!(
            !ctx.eval::<bool>(lock, "globalThis.after").unwrap(),
            "{call}: JavaScript ran on after the termination"
        );
        check(lock, ctx);
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

/// With `fast_jsg_struct`, the fields are defined as data properties, which V8 doesn't allow
/// while execution is terminating; without it, they are set.
#[test]
fn returning_a_struct_while_terminating_keeps_the_termination() {
    for fast in [false, true] {
        STRUCT_CONVERSION_PENDING.set(None);
        let harness = crate::Harness::with_flags(|mut flags| flags.set_fast_jsg_struct(fast));
        termination_survives_in(&harness, "terminateThenReturnStruct");
        assert_eq!(
            STRUCT_CONVERSION_PENDING.get(),
            Some(true),
            "fast_jsg_struct = {fast}"
        );
    }
}

#[test]
fn returning_a_resource_while_terminating_keeps_the_termination() {
    RESOURCE_CONVERSION_PENDING.set(None);
    termination_survives("terminateThenReturnResource");
    assert_eq!(RESOURCE_CONVERSION_PENDING.get(), Some(true));
}

#[test]
fn returning_a_wrapped_resource_while_terminating_keeps_the_termination() {
    THIS_CONVERSION_PENDING.set(None);
    termination_survives("terminateThenReturnThis");
    assert_eq!(THIS_CONVERSION_PENDING.get(), Some(true));
}

#[test]
fn returning_a_big_int_while_terminating_keeps_the_termination() {
    termination_survives("terminateThenReturnBigInt");
}

/// `terminate_execution_now()` delivers the termination before it returns, unlike
/// `terminate_execution()`, so the method returns its error with the termination pending, and
/// none of the caller's JavaScript runs after the call.
#[test]
fn terminate_execution_now_stops_javascript_at_once() {
    TERMINATE_NOW_RETURNED.set(false);
    termination_survives("terminateNow");
    assert!(TERMINATE_NOW_RETURNED.get());
}

#[test]
fn throw_error_after_terminate_execution_now_keeps_the_termination() {
    termination_survives("terminateNowThenThrowError");
}

#[test]
fn terminate_execution_now_while_terminating_keeps_the_termination() {
    termination_survives("terminateNowTwice");
}

#[test]
fn terminate_execution_now_replaces_a_pending_exception() {
    termination_survives("throwErrorThenTerminateNow");
}

/// A resource type that nothing uses before `terminateThenGetConstructor`, so that getting its
/// constructor creates its template.
#[jsg_resource]
struct Fresh;

#[jsg_resource]
impl Fresh {}

/// Is terminated and then uses a shim that checks `exceptionPending()` (`ffi.c++`) before it
/// calls a V8 API that takes a context. The operation fails with a pending error, calls nothing
/// in V8, and the termination keeps propagating. A call, a `Date`, a resource's wrapper and the
/// conversions are tested above, in `resource_callback.rs` and in `coercion_safety.rs`.
#[jsg_resource]
struct Guarded {
    /// Whether the operation failed with a pending error.
    saw_pending: Cell<Option<bool>>,
}

#[jsg_resource]
impl Guarded {
    fn record<T>(&self, result: &Result<T, jsg::Error>) {
        self.saw_pending
            .set(Some(result.as_ref().is_err_and(jsg::Error::is_pending)));
    }

    /// Is terminated and then gets the constructor of a resource type, which creates the
    /// type's template (fine while terminating) and then its constructor function (not fine:
    /// `FunctionTemplate::GetFunction` prepares to run JavaScript).
    #[jsg_method]
    fn terminate_then_get_constructor(&self, lock: &mut jsg::Lock) -> Result<(), jsg::Error> {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::resource::function_template_of::<Fresh>(lock).map(drop);
        self.record(&result);
        result
    }

    /// Is terminated and then converts `value` with `Utf8Value`, whose `toString` must not run.
    #[jsg_method]
    fn terminate_then_utf8_value<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: jsg::v8::Local<'s, jsg::v8::Value>,
    ) {
        crate::Harness::terminate_and_deliver(lock);
        let result = jsg::v8::Utf8Value::new(lock, &value);
        self.record(&result);
    }

    /// Is terminated and then detaches a new `ArrayBuffer`, which leaves it attached:
    /// `ArrayBuffer::Detach` runs no JavaScript, but V8 does not allow entering it while
    /// execution is terminating.
    #[jsg_method]
    fn terminate_then_detach(&self, lock: &mut jsg::Lock) -> Result<(), jsg::Error> {
        crate::Harness::terminate_and_deliver(lock);
        let buffer = jsg::v8::ArrayBuffer::new(lock, &[1u8]);
        let result = buffer.detach(lock);
        assert!(!buffer.was_detached());
        self.record(&result);
        result
    }
}

/// Runs `call` on a `Guarded` as `termination_survives_call` does, and checks that the operation
/// saw a pending error and that `check` holds afterwards.
fn guarded_termination_survives(
    call: &str,
    check: impl FnOnce(&mut jsg::Lock<'_>, &crate::EvalContext<'_>),
) {
    let guarded = jsg::Rc::new(Guarded {
        saw_pending: Cell::new(None),
    });
    termination_survives_call(&crate::Harness::new(), guarded.clone(), call, check);
    assert_eq!(guarded.saw_pending.get(), Some(true), "{call}");
}

#[test]
fn getting_a_constructor_while_terminating_keeps_the_termination() {
    guarded_termination_survives("terminateThenGetConstructor()", |_, _| {});
}

#[test]
fn detaching_while_terminating_keeps_the_termination() {
    guarded_termination_survives("terminateThenDetach()", |_, _| {});
}

#[test]
fn utf8_value_while_terminating_runs_nothing() {
    guarded_termination_survives(
        "terminateThenUtf8Value({ toString() { globalThis.stringified = true; return 'x'; } })",
        |lock, ctx| {
            assert!(
                !ctx.eval::<bool>(lock, "globalThis.stringified === true")
                    .unwrap()
            );
        },
    );
}
