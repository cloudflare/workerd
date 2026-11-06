// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_callable]` behaves as C++ JSG's `JSG_CALLABLE`. The first test mirrors the
//! "Test `JSG_CALLABLE`" test in `src/workerd/jsg/jsg-test.c++` case for case.

use std::cell::Cell;

use jsg::ExceptionType;
use jsg::MemberRegistry;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_callable;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

/// `MyCallable` of the C++ test.
#[jsg_resource]
struct MyCallable;

#[jsg_resource]
impl MyCallable {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self
    }

    #[jsg_callable]
    #[jsg_method]
    fn foo(&self) -> bool {
        let _ = self;
        true
    }
}

/// Adds its arguments to a running total, which it returns.
#[jsg_resource]
struct Counter {
    total: Cell<f64>,
}

#[jsg_resource]
impl Counter {
    #[jsg_callable]
    fn add(&self, lock: &mut jsg::Lock, amount: f64, times: Option<f64>) -> f64 {
        let _ = lock;
        self.total
            .set(amount.mul_add(times.unwrap_or(1.0), self.total.get()));
        self.total.get()
    }

    #[jsg_method]
    fn total(&self) -> f64 {
        self.total.get()
    }
}

fn counter(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Counter> {
    jsg::Rc::new(
        lock,
        Counter {
            total: Cell::new(0.0),
        },
    )
}

/// Returns itself when called, and fails when called with `"fail"`.
#[jsg_resource]
struct Itself;

#[jsg_resource]
impl Itself {
    #[jsg_callable]
    fn call(this: jsg::Rc<Self>, arg: Option<String>) -> Result<jsg::Rc<Self>, jsg::Error> {
        if arg.as_deref() == Some("fail") {
            return Err(jsg::Error::new_range_error("failed"));
        }
        Ok(this)
    }
}

/// Like an RPC stub: has every property, and calling it, or a property, records the path.
#[jsg_resource]
struct Stub {
    path: String,
}

#[jsg_resource]
impl Stub {
    #[jsg_callable]
    fn call(&self, arg: String) -> String {
        format!("{}({arg})", self.path)
    }

    #[jsg_wildcard_property]
    #[expect(
        clippy::unnecessary_wraps,
        reason = "a wildcard property returns an Option"
    )]
    fn get(&self, lock: &mut jsg::Lock, name: &str) -> Option<jsg::Rc<Self>> {
        Some(jsg::Rc::new(
            lock,
            Self {
                path: format!("{}.{name}", self.path),
            },
        ))
    }
}

/// Is callable one of two ways, depending on a compatibility flag.
#[jsg_resource]
struct Versioned;

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_callable]
    fn call_old(&self) -> String {
        let _ = self;
        "old".to_owned()
    }

    #[jsg_callable]
    fn call_new(&self) -> String {
        let _ = self;
        "new".to_owned()
    }
}

impl Resource for Versioned {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        if flags.get_cache_option_enabled() {
            registry.add(Self::call_new_callable_member());
        } else {
            registry.add(Self::call_old_callable_member());
        }
    }
}

/// Not callable.
#[jsg_resource]
struct Plain;

#[jsg_resource]
impl Plain {
    #[jsg_method]
    fn foo(&self) -> bool {
        let _ = self;
        true
    }
}

fn with_globals(f: impl for<'s, 'c> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'c>)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let my_callable = jsg::resource::function_template_of::<MyCallable>(lock).unwrap();
        ctx.set_global(lock, "MyCallable", my_callable);
        let get_callable = jsg::Rc::new(lock, MyCallable);
        ctx.set_global(lock, "getCallable", get_callable);
        let counter = counter(lock);
        ctx.set_global(lock, "counter", counter);
        let itself = jsg::Rc::new(lock, Itself);
        ctx.set_global(lock, "itself", itself);
        let stub = jsg::Rc::new(
            lock,
            Stub {
                path: "stub".to_owned(),
            },
        );
        ctx.set_global(lock, "stub", stub);
        let plain = jsg::Rc::new(lock, Plain);
        ctx.set_global(lock, "plain", plain);
        f(lock, ctx);
        Ok(())
    });
}

fn expect_eval(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str, expected: &str) {
    let result: String = ctx.eval(lock, code).unwrap();
    assert_eq!(result, expected, "{code}");
}

fn expect_error(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    code: &str,
    name: ExceptionType,
) -> String {
    let err = ctx
        .eval::<String>(lock, code)
        .unwrap_err()
        .unwrap_jsg_err(lock);
    assert_eq!(err.name, name, "{code}: {}", err.message);
    err.message
}

// =============================================================================
// jsg-test.c++ "Test JSG_CALLABLE"
// =============================================================================

// `getCallable` is a global instance here rather than a method returning one.
#[test]
fn callable_resource() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "let obj = getCallable; String(obj.foo())",
            "true",
        );
        expect_eval(lock, ctx, "String(getCallable())", "true");
        expect_eval(lock, ctx, "String(new MyCallable()())", "true");
        // Calling with `new` runs the method too.
        expect_eval(lock, ctx, "String(new getCallable())", "true");
    });
}

// =============================================================================
// The callable object
// =============================================================================

// V8 reports an object with a call handler as a function, but it is still an instance of the
// resource type, with the type's prototype rather than `Function.prototype`.
#[test]
fn callable_is_a_function_and_an_instance() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "typeof getCallable", "function");
        expect_eval(
            lock,
            ctx,
            "String(getCallable instanceof MyCallable)",
            "true",
        );
        expect_eval(
            lock,
            ctx,
            "String(Object.getPrototypeOf(getCallable) === MyCallable.prototype)",
            "true",
        );
        expect_eval(
            lock,
            ctx,
            "String(getCallable instanceof Function)",
            "false",
        );
        expect_eval(lock, ctx, "typeof getCallable.call", "undefined");
        expect_eval(
            lock,
            ctx,
            "String(Reflect.apply(getCallable, null, []))",
            "true",
        );
    });
}

#[test]
fn resource_without_jsg_callable_is_not_callable() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "typeof plain", "object");
        let message = expect_error(lock, ctx, "plain()", ExceptionType::TypeError);
        assert!(message.contains("is not a function"), "{message}");
    });
}

#[test]
fn custom_members_choose_how_it_is_callable() {
    for (enabled, expected) in [(false, "old"), (true, "new")] {
        let harness =
            crate::Harness::with_flags(|mut flags| flags.set_cache_option_enabled(enabled));
        harness.run_in_context(|lock, ctx| {
            let v = jsg::Rc::new(lock, Versioned);
            ctx.set_global(lock, "v", v);
            expect_eval(lock, ctx, "v()", expected);
            Ok(())
        });
    }
}

// =============================================================================
// `this`
// =============================================================================

// V8 runs the call handler with the callable as `this`, however the call names it, so a
// callable taken off an object still calls the instance.
#[test]
fn this_is_always_the_callable() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "String(itself() === itself)", "true");
        expect_eval(
            lock,
            ctx,
            "const o = { f: itself }; String(o.f() === itself)",
            "true",
        );
        expect_eval(
            lock,
            ctx,
            "String(Reflect.apply(itself, {}, []) === itself)",
            "true",
        );
        expect_eval(lock, ctx, "String(new itself() === itself)", "true");
        expect_eval(
            lock,
            ctx,
            "String(Object.create(itself).constructor === itself.constructor)",
            "true",
        );
    });
}

// An object that inherits from the callable is not callable itself.
#[test]
fn inheriting_object_is_not_callable() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "typeof Object.create(itself)", "object");
        expect_error(
            lock,
            ctx,
            "Object.create(itself)()",
            ExceptionType::TypeError,
        );
    });
}

// =============================================================================
// Arguments and results
// =============================================================================

#[test]
fn arguments_are_converted_as_for_a_method() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "String(counter(2))", "2");
        expect_eval(lock, ctx, "String(counter(3, 2))", "8");
        expect_eval(lock, ctx, "String(counter.total())", "8");
        // Extra arguments are ignored.
        expect_eval(lock, ctx, "String(counter(1, 1, 'extra'))", "9");
    });
}

// Errors name the method as C++ JSG's `JSG_CALLABLE` does.
#[test]
fn argument_errors_name_the_call() {
    with_globals(|lock, ctx| {
        assert_eq!(
            expect_error(lock, ctx, "counter()", ExceptionType::TypeError),
            "Failed to execute '(called as function)' on 'Counter': parameter 1 is not of type \
             'number'."
        );
        assert_eq!(
            expect_error(lock, ctx, "stub(Symbol())", ExceptionType::TypeError),
            "Cannot convert a Symbol value to a string"
        );
        expect_eval(lock, ctx, "String(counter.total())", "0");
    });
}

#[test]
fn error_is_thrown() {
    with_globals(|lock, ctx| {
        assert_eq!(
            expect_error(lock, ctx, "itself('fail')", ExceptionType::RangeError),
            "failed"
        );
        expect_eval(lock, ctx, "String(itself() === itself)", "true");
    });
}

// =============================================================================
// With a wildcard property
// =============================================================================

// A callable resource with a wildcard property has the shape of a JS RPC stub: properties of
// any name, each callable in turn.
#[test]
fn callable_with_wildcard_property() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "stub('x')", "stub(x)");
        expect_eval(lock, ctx, "stub.foo('x')", "stub.foo(x)");
        expect_eval(lock, ctx, "stub.foo.bar('y')", "stub.foo.bar(y)");
        expect_eval(lock, ctx, "typeof stub.foo", "function");
        expect_eval(
            lock,
            ctx,
            "const m = stub.method; m('torn off')",
            "stub.method(torn off)",
        );
    });
}
