// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `v8::Function::new` creates a JavaScript function from a Rust closure and the data it owns,
//! like C++ JSG's `Lock::wrapReturningFunction()`.

use std::cell::Cell;
use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Creates a function whose data is `greeting` and stores it as the global `greet`.
fn install_greet(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, greeting: &str) {
    let greet = v8::Function::new(lock, greeting.to_owned(), |lock, greeting, args| {
        let name = String::from_js(lock, args.get(0))?;
        Ok(format!("{greeting}, {name}").to_js(lock)?)
    })
    .unwrap();
    ctx.set_global(lock, "greet", v8::Local::<v8::Value>::from(greet));
}

/// Returns `"Name: message"` for what `code` throws, or `"ok"`.
fn error(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    let code = format!("try {{ {code}; 'ok' }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}");
    ctx.eval(lock, &code).unwrap()
}

#[test]
fn function_calls_closure_with_its_data_and_arguments() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_greet(lock, ctx, "hello");
        assert_eq!(
            ctx.eval::<String>(lock, "greet('world')").unwrap(),
            "hello, world"
        );
        Ok(())
    });
}

#[test]
fn functions_keep_their_own_data() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let hello = v8::Function::new(lock, "hello".to_owned(), |lock, data, _args| {
            Ok(data.clone().to_js(lock)?)
        })
        .unwrap();
        let count = v8::Function::new(lock, Cell::new(0u32), |lock, calls, _args| {
            calls.set(calls.get() + 1);
            Ok(jsg::Number::from(calls.get()).to_js(lock)?)
        })
        .unwrap();
        ctx.set_global(lock, "hello", v8::Local::<v8::Value>::from(hello));
        ctx.set_global(lock, "count", v8::Local::<v8::Value>::from(count));
        assert_eq!(
            ctx.eval::<String>(lock, "[hello(), count(), count(), hello(), count()].join()")
                .unwrap(),
            "hello,1,2,hello,3"
        );
        Ok(())
    });
}

#[test]
fn function_sees_its_receiver() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let this = v8::Function::new(lock, (), |_lock, (), args| Ok(args.this())).unwrap();
        ctx.set_global(lock, "getThis", v8::Local::<v8::Value>::from(this));
        assert!(
            ctx.eval::<bool>(lock, "const o = {}; getThis.call(o) === o")
                .unwrap()
        );
        Ok(())
    });
}

#[test]
fn function_throws_closure_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_greet(lock, ctx, "hello");
        let fail = v8::Function::new(lock, (), |_lock, (), _args| {
            Err(jsg::Error::new_range_error("out of range"))
        })
        .unwrap();
        ctx.set_global(lock, "fail", v8::Local::<v8::Value>::from(fail));
        assert_eq!(error(lock, ctx, "fail()"), "RangeError: out of range");
        // A JavaScript exception raised while converting an argument propagates.
        assert_eq!(
            error(
                lock,
                ctx,
                "greet({ toString() { throw new TypeError('no name'); } })"
            ),
            "TypeError: no name"
        );
        Ok(())
    });
}

#[test]
fn panic_in_closure_is_an_internal_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let panics = v8::Function::new(lock, (), |_lock, (), _args| -> Result<_, jsg::Error> {
            panic!("intentional panic in a function closure");
        })
        .unwrap();
        ctx.set_global(lock, "panics", v8::Local::<v8::Value>::from(panics));
        let err = ctx
            .eval::<bool>(lock, "panics()")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert!(err.message.contains("internal error"), "{}", err.message);
        assert!(
            !err.message.contains("intentional panic"),
            "{}",
            err.message
        );
        Ok(())
    });
}

#[test]
fn function_shape_matches_cpp_jsg() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_greet(lock, ctx, "hello");
        let code = "[typeof greet, JSON.stringify(greet.name), greet.length, \
                    typeof new greet('x')].join()";
        assert_eq!(
            ctx.eval::<String>(lock, code).unwrap(),
            "function,\"\",0,object"
        );
        Ok(())
    });
}

#[test]
fn data_of_cpp_wrapped_function_is_not_taken_for_rust_data() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        // Functions that C++ JSG wraps also have an opaque wrapper as their data, holding a C++
        // Wrappable that must not be read as a Rust one.
        assert!(!crate::Harness::cpp_wrapped_function_data_is_rust_wrappable(lock));
        Ok(())
    });
}

// =============================================================================
// Garbage collection
// =============================================================================

/// Counts its drops in the counter it points to.
struct DropCounter(&'static AtomicUsize);

impl Drop for DropCounter {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

impl jsg::Traced for DropCounter {}

static UNREACHABLE_DROPS: AtomicUsize = AtomicUsize::new(0);

#[test]
fn data_is_dropped_once_function_is_unreachable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let function = v8::Function::new(lock, DropCounter(&UNREACHABLE_DROPS), |lock, _, _| {
            Ok(lock.undefined())
        })
        .unwrap();
        ctx.set_global(lock, "f", v8::Local::<v8::Value>::from(function));
        crate::Harness::request_gc(lock);
        assert_eq!(UNREACHABLE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(UNREACHABLE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Terminates execution, then creates a function.
#[jsg_resource]
struct TerminatingCreator {
    /// Whether `Function::new` failed with a pending error.
    saw_pending: Cell<bool>,
}

#[jsg_resource]
impl TerminatingCreator {
    /// Requests a termination, which V8 delivers while `spin` runs, then creates a function.
    #[jsg_method]
    fn create_after_termination<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        spin: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        lock.terminate_execution();
        let spin: v8::Local<v8::Function> = spin.try_into().unwrap();
        let err = spin
            .call::<v8::Local<v8::Value>>(lock, None, &[])
            .unwrap_err();
        assert!(err.is_pending());
        let err = v8::Function::new(lock, (), |lock, (), _args| Ok(lock.undefined())).unwrap_err();
        self.saw_pending.set(err.is_pending());
        Err(err)
    }
}

/// While execution is terminating, `Function::new` creates nothing and returns a pending error,
/// and returning it keeps the termination: JavaScript cannot catch it or run on.
#[test]
fn function_new_while_terminating_keeps_the_termination() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let creator = jsg::Rc::new(TerminatingCreator {
            saw_pending: Cell::new(false),
        });
        ctx.set_global(lock, "creator", creator.clone());
        let result = ctx.eval_raw(
            lock,
            "globalThis.caught = false; globalThis.after = false;
             try { creator.createAfterTermination(() => { while (true) {} }); }
             catch (e) { globalThis.caught = true; }
             globalThis.after = true;",
        );
        assert!(result.is_err(), "the script was not terminated");
        assert!(creator.saw_pending.get());
        assert!(!ctx.eval::<bool>(lock, "globalThis.caught").unwrap());
        assert!(!ctx.eval::<bool>(lock, "globalThis.after").unwrap());
        Ok(())
    });
}

/// Calls a function that throws, then creates a function.
#[jsg_resource]
struct ThrowingCreator {
    /// Whether `Function::new` failed with a pending error.
    saw_pending: Cell<bool>,
}

#[jsg_resource]
impl ThrowingCreator {
    /// Calls `throws`, which leaves its exception pending, then creates a function.
    #[jsg_method]
    fn create_after_throw<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        throws: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let throws: v8::Local<v8::Function> = throws.try_into().unwrap();
        let err = throws
            .call::<v8::Local<v8::Value>>(lock, None, &[])
            .unwrap_err();
        assert!(err.is_pending());
        let err = v8::Function::new(lock, (), |lock, (), _args| Ok(lock.undefined())).unwrap_err();
        self.saw_pending.set(err.is_pending());
        Err(err)
    }
}

/// While an exception is pending, `Function::new` creates nothing and returns a pending error,
/// and returning it keeps the exception, which JavaScript then catches unchanged.
#[test]
fn function_new_while_exception_pending_keeps_the_exception() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let creator = jsg::Rc::new(ThrowingCreator {
            saw_pending: Cell::new(false),
        });
        ctx.set_global(lock, "creator", creator.clone());
        assert_eq!(
            ctx.eval::<String>(
                lock,
                "const thrown = new TypeError('thrown');
                 let caught;
                 try { creator.createAfterThrow(() => { throw thrown; }); }
                 catch (e) { caught = e; }
                 caught === thrown ? 'same' : String(caught)",
            )
            .unwrap(),
            "same"
        );
        assert!(creator.saw_pending.get());
        Ok(())
    });
}

static LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct Leaf {
    _drops: DropCounter,
}

#[jsg_resource]
impl Leaf {}

#[test]
fn data_keeps_traced_resources_alive() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let leaf = jsg::Rc::new(Leaf {
            _drops: DropCounter(&LEAF_DROPS),
        });
        let function =
            v8::Function::new(lock, leaf, |lock, leaf, _| Ok(leaf.clone().to_js(lock)?)).unwrap();
        ctx.set_global(lock, "getLeaf", v8::Local::<v8::Value>::from(function));
        // Only the function's data holds the resource now.
        crate::Harness::request_gc(lock);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 0);
        assert!(ctx.eval::<bool>(lock, "getLeaf() === getLeaf()").unwrap());
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

static CYCLE_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Data that can hold a handle back to its own function.
struct CycleData {
    _drops: DropCounter,
    stored: RefCell<Option<v8::TracedGlobal<v8::Value>>>,
}

impl jsg::Traced for CycleData {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.stored.trace(visitor);
    }
}

#[test]
fn cycle_through_data_is_collected() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let data = CycleData {
            _drops: DropCounter(&CYCLE_DROPS),
            stored: RefCell::new(None),
        };
        let function = v8::Function::new(lock, data, |lock, data, args| {
            *data.stored.borrow_mut() = Some(args.get(0).to_traced_global(lock));
            Ok(lock.undefined())
        })
        .unwrap();
        ctx.set_global(lock, "store", v8::Local::<v8::Value>::from(function));
        // The data now holds the function that owns it.
        ctx.eval_raw(lock, "store(store)").unwrap();
        crate::Harness::request_gc(lock);
        assert_eq!(CYCLE_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CYCLE_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn reentrant_call_sees_its_own_data() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        // Each call logs its depth on entry and exit, and calls `again` in between, which calls
        // the same function again. No `RefCell` borrow is held across the call into JavaScript.
        let f = v8::Function::new(lock, RefCell::new(Vec::new()), |lock, log, args| {
            let depth = jsg::Number::from_js(lock, args.get(0))?.value();
            log.borrow_mut().push(format!("+{depth}"));
            if depth > 0.0 {
                let again: v8::Local<v8::Function> = args.get(1).try_into().unwrap();
                let next = jsg::Number::from(depth - 1.0).to_js(lock)?;
                again.call::<v8::Local<v8::Value>>(lock, None, &[next])?;
            }
            log.borrow_mut().push(format!("-{depth}"));
            Ok(log.borrow().join(" ").to_js(lock)?)
        })
        .unwrap();
        ctx.set_global(lock, "f", v8::Local::<v8::Value>::from(f));
        assert_eq!(
            ctx.eval::<String>(lock, "const again = (n) => f(n, again); f(2, again)")
                .unwrap(),
            "+2 +1 +0 -0 -1 -2"
        );
        Ok(())
    });
}

static TEARDOWN_DATA_DROPS: AtomicUsize = AtomicUsize::new(0);
static TEARDOWN_LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Data that holds a resource and a handle back to its own function.
struct TeardownData {
    _drops: DropCounter,
    leaf: jsg::Rc<Leaf>,
    stored: RefCell<Option<v8::TracedGlobal<v8::Value>>>,
}

impl jsg::Traced for TeardownData {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.leaf.trace(visitor);
        self.stored.trace(visitor);
    }
}

/// The data of a function that is still reachable when the isolate is torn down is dropped by
/// the teardown, together with the resource and the `TracedGlobal` it owns.
#[test]
fn data_of_reachable_function_is_dropped_with_its_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let data = TeardownData {
            _drops: DropCounter(&TEARDOWN_DATA_DROPS),
            leaf: jsg::Rc::new(Leaf {
                _drops: DropCounter(&TEARDOWN_LEAF_DROPS),
            }),
            stored: RefCell::new(None),
        };
        let function = v8::Function::new(lock, data, |lock, data, args| {
            *data.stored.borrow_mut() = Some(args.get(0).to_traced_global(lock));
            Ok(lock.undefined())
        })
        .unwrap();
        ctx.set_global(lock, "store", v8::Local::<v8::Value>::from(function));
        ctx.eval_raw(lock, "store(store)").unwrap();
        // Tracing puts the `TracedGlobal` in traced mode; the global object keeps the function.
        crate::Harness::request_gc(lock);
        assert_eq!(TEARDOWN_DATA_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(TEARDOWN_LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });
    drop(harness);
    assert_eq!(TEARDOWN_DATA_DROPS.load(Ordering::SeqCst), 1);
    assert_eq!(TEARDOWN_LEAF_DROPS.load(Ordering::SeqCst), 1);
}
