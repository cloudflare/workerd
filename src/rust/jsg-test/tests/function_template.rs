// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `v8::FunctionTemplate::new` and `Local<FunctionTemplate>::get_function`: native functions
//! without data, as C++ `v8::FunctionTemplate::New()` and `GetFunction()` make them, with an
//! optional fast callback from `jsg::fast_api::function`.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::fast_api;
use jsg::v8;
use jsg::v8::ffi::ConstructorBehavior;
use jsg::v8::ffi::SideEffectType;

fn double() -> jsg::RawFunctionCallback {
    jsg::function_callback(|lock, args| {
        let x = f64::from_js(lock, args.get(0)).unwrap_or(f64::NAN);
        args.set_return_number(x * 2.0);
    })
}

/// Makes the function of a template with `callback` and `fast`, and stores it as the global
/// `name`.
fn install(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    name: &str,
    callback: jsg::RawFunctionCallback,
    behavior: ConstructorBehavior,
    fast: Option<fast_api::FastCallback>,
) {
    let function = v8::FunctionTemplate::new(
        lock,
        callback,
        behavior,
        SideEffectType::HasSideEffect,
        fast,
    )
    .get_function(lock)
    .unwrap();
    ctx.set_global(lock, name, v8::Local::<v8::Value>::from(function));
}

/// Defines `hot(...)` with `body`, calls it once with `warmup` so V8 collects feedback, optimizes
/// it, and returns the source of the call `hot(args)`.
fn optimized(params: &str, body: &str, warmup: &str, args: &str) -> String {
    format!(
        "function hot({params}) {{ {body} }}
         %PrepareFunctionForOptimization(hot);
         hot({warmup});
         %OptimizeFunctionOnNextCall(hot);
         hot({args})"
    )
}

/// Runs `f` and counts the fast callbacks it ran, or returns `None` for the count in builds
/// without debug assertions, which don't count them.
#[cfg(debug_assertions)]
fn count_fast_calls<T>(f: impl FnOnce() -> T) -> (T, Option<u64>) {
    let before = fast_api::call_count();
    let result = f();
    (result, Some(fast_api::call_count() - before))
}

#[cfg(not(debug_assertions))]
fn count_fast_calls<T>(f: impl FnOnce() -> T) -> (T, Option<u64>) {
    (f(), None)
}

#[test]
fn function_calls_its_callback() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(
            lock,
            ctx,
            "double",
            double(),
            ConstructorBehavior::Throw,
            None,
        );
        assert_eq!(ctx.eval::<i32>(lock, "double(21)").unwrap(), 42);
        Ok(())
    });
}

#[test]
fn get_function_returns_one_function_per_context() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let template = v8::FunctionTemplate::new(
            lock,
            double(),
            ConstructorBehavior::Throw,
            SideEffectType::HasNoSideEffect,
            None,
        );
        let first = template.get_function(lock)?;
        let second = template.get_function(lock)?;
        ctx.set_global(lock, "first", v8::Local::<v8::Value>::from(first));
        ctx.set_global(lock, "second", v8::Local::<v8::Value>::from(second));
        assert!(ctx.eval::<bool>(lock, "first === second").unwrap());
        Ok(())
    });
}

#[test]
fn constructor_behavior_decides_whether_new_works() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(
            lock,
            ctx,
            "plain",
            double(),
            ConstructorBehavior::Throw,
            None,
        );
        install(
            lock,
            ctx,
            "ctor",
            double(),
            ConstructorBehavior::Allow,
            None,
        );
        assert_eq!(
            ctx.eval::<String>(lock, "try { new plain(1); 'ok' } catch (e) { e.name }")
                .unwrap(),
            "TypeError"
        );
        assert_eq!(
            ctx.eval::<String>(lock, "typeof new ctor(1)").unwrap(),
            "object"
        );
        // A function that is not a constructor has no prototype property.
        assert!(
            ctx.eval::<bool>(lock, "!('prototype' in plain) && 'prototype' in ctor")
                .unwrap()
        );
        Ok(())
    });
}

fn is_array_buffer(value: v8::Local<'_, v8::Value>) -> bool {
    value.is_array_buffer()
}

#[test]
fn optimized_code_calls_the_fast_callback() {
    crate::Harness::with_fast_api().run_in_context(|lock, ctx| {
        install(
            lock,
            ctx,
            "isArrayBuffer",
            jsg::function_callback(|_lock, args| {
                let value = args.get(0);
                args.set_return_boolean(is_array_buffer(value));
            }),
            ConstructorBehavior::Throw,
            Some(fast_api::function(is_array_buffer)),
        );
        let (result, fast_calls) = count_fast_calls(|| {
            ctx.eval::<String>(
                lock,
                &optimized(
                    "x",
                    "return isArrayBuffer(x);",
                    "1",
                    "new ArrayBuffer(1)) + ',' + hot(new Uint8Array(1)",
                ),
            )
            .unwrap()
        });
        assert_eq!(result, "true,false");
        assert_ne!(fast_calls, Some(0));
        Ok(())
    });
}

static CALLS: AtomicUsize = AtomicUsize::new(0);

fn count_call(_value: v8::Local<'_, v8::Value>) {
    CALLS.fetch_add(1, Ordering::SeqCst);
}

#[test]
fn fast_callback_may_return_nothing() {
    crate::Harness::with_fast_api().run_in_context(|lock, ctx| {
        install(
            lock,
            ctx,
            "countCall",
            jsg::function_callback(|_lock, args| count_call(args.get(0))),
            ConstructorBehavior::Throw,
            Some(fast_api::function(count_call)),
        );
        let before = CALLS.load(Ordering::SeqCst);
        let (result, fast_calls) = count_fast_calls(|| {
            ctx.eval::<bool>(
                lock,
                &optimized(
                    "x",
                    "return countCall(x);",
                    "1",
                    "2) === undefined && hot(3",
                ),
            )
            .unwrap()
        });
        assert!(!result, "hot() returns what countCall() does: undefined");
        // The warmup and both optimized calls, through either callback.
        assert_eq!(CALLS.load(Ordering::SeqCst) - before, 3);
        assert_ne!(fast_calls, Some(0));
        Ok(())
    });
}

#[test]
fn fast_callback_is_ignored_without_fast_calls() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(
            lock,
            ctx,
            "isArrayBuffer",
            jsg::function_callback(|_lock, args| {
                let value = args.get(0);
                args.set_return_boolean(is_array_buffer(value));
            }),
            ConstructorBehavior::Throw,
            Some(fast_api::function(is_array_buffer)),
        );
        let (result, fast_calls) = count_fast_calls(|| {
            ctx.eval::<bool>(
                lock,
                &optimized("x", "return isArrayBuffer(x);", "1", "new ArrayBuffer(1)"),
            )
            .unwrap()
        });
        assert!(result);
        assert!(matches!(fast_calls, Some(0) | None));
        Ok(())
    });
}

#[test]
fn function_survives_a_startup_snapshot() {
    crate::snapshot_round_trip(
        |lock, ctx| {
            install(
                lock,
                ctx,
                "double",
                double(),
                ConstructorBehavior::Throw,
                None,
            );
            Ok(())
        },
        |lock, ctx| {
            assert_eq!(ctx.eval::<i32>(lock, "double(4)").unwrap(), 8);
            Ok(())
        },
    )
    .unwrap();
}
