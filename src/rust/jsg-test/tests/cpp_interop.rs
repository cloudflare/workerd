// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `jsg_cpp`: a Rust resource that takes, returns and holds C++ resources, and C++
//! resources that take, return and hold it (`crate::cpp_interop`).

use std::cell::Cell;
use std::cell::RefCell;

use jsg::v8;

use crate::Harness;
use crate::cpp_interop::HOST_DROPS;
use crate::cpp_interop::Host;
use crate::cpp_interop::ffi;

/// Sets the simulated request for the duration of a test.
struct InRequest;

impl InRequest {
    fn enter() -> Self {
        ffi::test_cpp_set_in_request(true);
        Self
    }
}

impl Drop for InRequest {
    fn drop(&mut self) {
        ffi::test_cpp_set_in_request(false);
    }
}

fn new_host() -> jsg::Rc<Host> {
    jsg::Rc::new(Host {
        label: "test".to_owned(),
        held: RefCell::new(None),
    })
}

/// Runs a C++ constructor and returns the wrapper it made.
fn new_cpp<'s>(
    lock: &jsg::Lock<'s>,
    constructor: impl FnOnce(*mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal,
) -> v8::Local<'s, v8::Value> {
    let value = constructor(lock.isolate().as_ffi());
    // SAFETY: the constructor returned a handle in the scope `lock` stands for.
    unsafe { v8::Local::from_ffi_maybe(value) }.expect("the C++ constructor threw")
}

/// Sets the globals `h` (a `Host`), `greeter`, `derived` and `holder`.
fn set_globals(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, host: jsg::Rc<Host>) {
    ctx.set_global(lock, "h", host);
    // SAFETY: the constructors are called with the lock's isolate, in its scope.
    let greeter = new_cpp(lock, |isolate| unsafe {
        ffi::test_cpp_greeter_new(isolate, "Hello")
    });
    ctx.set_global(lock, "greeter", greeter);
    // SAFETY: as above.
    let derived = new_cpp(lock, |isolate| unsafe {
        ffi::test_cpp_derived_new(isolate)
    });
    ctx.set_global(lock, "derived", derived);
    // SAFETY: as above.
    let holder = new_cpp(lock, |isolate| unsafe { ffi::test_cpp_holder_new(isolate) });
    ctx.set_global(lock, "holder", holder);
}

fn eval<T>(code: &str) -> T
where
    T: for<'a> jsg::FromJS<'a, ResultType = T> + Default,
{
    let mut out = T::default();
    Harness::new().run_in_context(|lock, ctx| {
        set_globals(lock, ctx, new_host());
        out = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    out
}

/// Runs `code`, which must throw, and returns the name and message of the exception.
fn eval_error(code: &str) -> (String, String) {
    let mut out = (String::new(), String::new());
    Harness::new().run_in_context(|lock, ctx| {
        set_globals(lock, ctx, new_host());
        let error = ctx
            .eval::<String>(lock, code)
            .unwrap_err()
            .unwrap_jsg_err(lock);
        out = (error.name.to_string(), error.message);
        Ok(())
    });
    out
}

#[test]
fn rust_method_takes_a_cpp_resource_and_calls_it() {
    assert_eq!(
        eval::<String>("h.greetWith(greeter, 'Rust')"),
        "Hello, Rust"
    );
}

#[test]
fn rust_method_rejects_another_type() {
    for argument in ["holder", "({})", "1", "undefined", "h"] {
        let (name, message) = eval_error(&format!("h.greetWith({argument}, 'x')"));
        assert_eq!(name, "TypeError", "{argument}: {message}");
    }
}

#[test]
fn rust_method_takes_a_cpp_subclass() {
    assert_eq!(eval::<String>("h.kindOf(derived)"), "derived");
}

#[test]
fn lazy_property_returns_the_cpp_resource_of_the_request() {
    let _request = InRequest::enter();
    assert_eq!(
        eval::<String>(
            "[h.current === h.current, h.current.kind(), h.current.extra(), \
              h.kindOf(h.current)].join()"
        ),
        "true,derived,only on the subclass,derived"
    );
}

#[test]
fn lazy_property_is_undefined_outside_a_request() {
    // `None` is `undefined`, as C++ `jsg::Optional` is, which `ctx.cache` returns.
    assert!(eval::<bool>("h.current === undefined"));
}

#[test]
fn lazy_property_keeps_the_first_value_once_the_request_ends() {
    // An `ExecutionContext` can outlive the request that created it, so a ported one has to
    // decide whether `lazy_instance` memoization is right for each property.
    Harness::new().run_in_context(|lock, ctx| {
        set_globals(lock, ctx, new_host());
        let request = InRequest::enter();
        assert!(
            ctx.eval::<bool>(lock, "globalThis.first = h.current; first !== undefined")
                .unwrap()
        );
        drop(request);
        assert!(ctx.eval::<bool>(lock, "h.current === first").unwrap());
        Ok(())
    });
}

#[test]
fn cpp_method_returns_the_rust_resource_it_was_given() {
    assert!(eval::<bool>("holder.hold(h); holder.getHeld() === h"));
}

#[test]
fn cpp_method_rejects_another_type_for_the_rust_resource() {
    for argument in ["greeter", "({})", "1"] {
        let (name, message) = eval_error(&format!("holder.hold({argument})"));
        assert_eq!(name, "TypeError", "{argument}: {message}");
    }
}

#[test]
fn cpp_method_calls_the_rust_resource() {
    assert_eq!(
        eval::<String>("holder.hold(h); holder.describeHeld()"),
        "Host(test)"
    );
}

#[test]
fn cpp_union_tells_the_rust_resource_from_a_cpp_one() {
    assert_eq!(
        eval::<String>("[holder.which(h), holder.which(greeter)].join()"),
        "Host,TestCppGreeter"
    );
    let (name, _) = eval_error("holder.which(holder)");
    assert_eq!(name, "TypeError");
}

#[test]
fn rust_field_returns_the_cpp_resource_it_was_given() {
    assert!(eval::<bool>("h.hold(holder); h.held() === holder"));
}

#[test]
fn cpp_created_rust_resource_is_usable() {
    let mut out = String::new();
    Harness::new().run_in_context(|lock, ctx| {
        // SAFETY: as in `set_globals`.
        let holder = new_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_holder_new_holding_host(isolate, "from C++")
        });
        ctx.set_global(lock, "holder", holder);
        out = ctx
            .eval(
                lock,
                "holder.getHeld().greetWith !== undefined && holder.describeHeld()",
            )
            .unwrap();
        Ok(())
    });
    assert_eq!(out, "Host(from C++)");
}

#[test]
fn kj_exception_from_cpp_becomes_its_js_error() {
    let (name, message) = eval_error("h.failWithTypeError(greeter)");
    assert_eq!(name, "TypeError");
    assert_eq!(message, "thrown by C++ as a kj::Exception");
}

#[test]
fn js_exception_from_cpp_passes_through() {
    let (name, message) = eval_error("h.failWithJsError(greeter)");
    assert_eq!(name, "RangeError");
    assert_eq!(message, "thrown by C++ as a JavaScript exception");
}

#[test]
fn cross_heap_cycle_is_collected() {
    HOST_DROPS.with(|drops| drops.set(0));
    let destroyed = ffi::test_cpp_holders_destroyed();
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_globals(lock, ctx, new_host());
        // A Rust resource and a C++ resource that hold each other, and nothing else holds.
        ctx.eval_raw(
            lock,
            "h.hold(holder); holder.hold(h); globalThis.h = undefined; \
             globalThis.holder = undefined;",
        )
        .unwrap();
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        Harness::request_gc(lock);
        Ok(())
    });
    assert_eq!(HOST_DROPS.with(Cell::get), 1);
    assert_eq!(ffi::test_cpp_holders_destroyed() - destroyed, 1);
}

#[test]
fn held_resources_survive_gc() {
    Harness::new().run_in_context(|lock, ctx| {
        set_globals(lock, ctx, new_host());
        ctx.eval_raw(
            lock,
            "h.hold(holder); holder.hold(h); globalThis.keep = h; globalThis.h = undefined; \
             globalThis.holder = undefined;",
        )
        .unwrap();
        Harness::request_gc(lock);
        assert_eq!(
            ctx.eval::<String>(
                lock,
                "keep.held().getHeld() === keep && keep.held().describeHeld()"
            )
            .unwrap(),
            "Host(test)"
        );
        Ok(())
    });
}
