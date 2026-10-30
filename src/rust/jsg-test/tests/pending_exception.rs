// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! While an ordinary exception is pending, an operation that would call a V8 API that takes a
//! context fails with a pending error and runs no JavaScript, and the exception reaches
//! JavaScript unchanged. See `src/rust/jsg/README.md` §Safety rules, rule 1.
//!
//! Each method first calls `first`, which throws, and then runs its operation. The conversions
//! and property access are tested in `coercion_safety.rs`, a call in `resource_callback.rs`, and
//! `Function::new` in `function_new.rs`.

use std::cell::Cell;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// A resource type whose template no test creates before it runs its operation (each test has
/// its own harness), so that getting its constructor creates the template.
#[jsg_resource]
struct Fresh;

#[jsg_resource]
impl Fresh {}

#[jsg_resource]
struct Thrower {
    /// Whether the operation failed with a pending error.
    saw_pending: Cell<Option<bool>>,
}

impl Thrower {
    /// Calls `first`, which must throw, then runs `operation`, records whether it failed with a
    /// pending error, and fails as `first` did.
    fn throw_then<'s, T>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
        operation: impl FnOnce(&mut jsg::Lock<'s>) -> jsg::Result<T>,
    ) -> jsg::Result<()> {
        let first = v8::Local::<v8::Function>::try_from(first)
            .unwrap()
            .call(lock, None, &[]);
        assert!(first.as_ref().is_err_and(jsg::Error::is_pending));
        let result = operation(lock);
        self.saw_pending
            .set(Some(result.is_err_and(|err| err.is_pending())));
        first.map(drop)
    }
}

#[jsg_resource]
impl Thrower {
    /// Creates a `Date` (`Date::New`).
    #[jsg_method]
    fn throw_then_create_date<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        self.throw_then(lock, first, |lock| v8::Date::new(lock, 0.0))
    }

    /// Wraps a new resource, which creates its wrapper (`ObjectTemplate::NewInstance`).
    #[jsg_method]
    fn throw_then_wrap_new<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        self.throw_then(lock, first, |lock| jsg::Rc::new(Fresh).to_js(lock))
    }

    /// Wraps its own resource, whose wrapper already exists, so that wrapping it only reads the
    /// wrapper. That fails too, as creating one would.
    #[jsg_method]
    fn throw_then_wrap_this<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        this.throw_then(lock, first, |lock| this.clone().to_js(lock))
    }

    /// Gets the constructor of a resource type (`FunctionTemplate::GetFunction`).
    #[jsg_method]
    fn throw_then_get_constructor<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        self.throw_then(lock, first, jsg::resource::function_template_of::<Fresh>)
    }

    /// Detaches a new `ArrayBuffer`, which leaves it attached.
    #[jsg_method]
    fn throw_then_detach<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        self.throw_then(lock, first, |lock| {
            let buffer = v8::ArrayBuffer::new(lock, &[1u8]);
            let result = buffer.detach(lock);
            assert!(!buffer.was_detached());
            result
        })
    }

    /// Converts `value` with `Utf8Value`, whose `toString` must not run.
    #[jsg_method]
    fn throw_then_utf8_value<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        first: v8::Local<'s, v8::Value>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        self.throw_then(lock, first, |lock| v8::Utf8Value::new(lock, &value))
    }
}

/// Evaluates `obj.{call}`, where `obj` is a `Thrower` and the call's first argument is
/// `first`, a function that throws. Checks that the operation failed with a pending error, that
/// JavaScript catches the exception `first` threw, and that no other JavaScript ran. Only the
/// `Utf8Value` case passes JavaScript that would set `secondRan`; for the others, the pending
/// error and the caught exception are what tell a guarded shim from one that isn't.
fn exception_survives(call: &str) {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let thrower = jsg::Rc::new(Thrower {
            saw_pending: Cell::new(None),
        });
        ctx.set_global(lock, "obj", thrower.clone());
        let caught = ctx
            .eval::<String>(
                lock,
                &format!(
                    "globalThis.secondRan = false;
                     const first = () => {{ throw new RangeError('first'); }};
                     try {{ obj.{call}; 'nothing' }} catch (e) {{ String(e); }}"
                ),
            )
            .unwrap();
        assert_eq!(caught, "RangeError: first", "{call}");
        assert_eq!(thrower.saw_pending.get(), Some(true), "{call}");
        assert!(
            !ctx.eval::<bool>(lock, "secondRan").unwrap(),
            "{call} ran JavaScript with an exception pending"
        );
        Ok(())
    });
}

#[test]
fn creating_a_date_while_exception_pending_fails() {
    exception_survives("throwThenCreateDate(first)");
}

#[test]
fn wrapping_a_new_resource_while_exception_pending_fails() {
    exception_survives("throwThenWrapNew(first)");
}

#[test]
fn wrapping_a_wrapped_resource_while_exception_pending_fails() {
    exception_survives("throwThenWrapThis(first)");
}

#[test]
fn getting_a_constructor_while_exception_pending_fails() {
    exception_survives("throwThenGetConstructor(first)");
}

#[test]
fn detaching_while_exception_pending_fails() {
    exception_survives("throwThenDetach(first)");
}

#[test]
fn utf8_value_while_exception_pending_runs_nothing() {
    exception_survives(
        "throwThenUtf8Value(first, { toString() { globalThis.secondRan = true; return 'x'; } })",
    );
}

#[cfg(debug_assertions)]
#[jsg_resource]
struct Callee {
    ran: Cell<bool>,
}

#[cfg(debug_assertions)]
#[jsg_resource]
impl Callee {
    #[jsg_method]
    fn run(&self) {
        self.ran.set(true);
    }
}

/// V8 entering a Rust JSG callback while a C++ `v8::TryCatch` holds a caught exception fails a
/// debug assertion, which terminates execution: every operation of the callback would fail
/// with a pending error.
#[cfg(debug_assertions)]
#[test]
fn callback_entered_while_try_catch_holds_exception_panics() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let callee = jsg::Rc::new(Callee {
            ran: Cell::new(false),
        });
        ctx.set_global(lock, "callee", callee.clone());
        match ctx.eval_with_caught_exception(lock, "callee.run()") {
            Err(crate::EvalError::EvalFailed) => {}
            Err(crate::EvalError::Exception(exception)) => assert!(exception.is_null()),
            other => panic!("the callback was entered: {other:?}"),
        }
        assert!(!callee.ran.get());
        Ok(())
    });
}
