// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::AsyncContextFrame` and `jsg::TracedAsyncContextFrame`: capturing the current async
//! context frame and running code in it later.
//!
//! The frames are C++ `jsg::AsyncContextFrame`s that `Harness::run_in_async_context` creates,
//! as `AsyncLocalStorage.run()` does, each storing a value under one test storage key. Rust
//! reads the current frame's value with `Harness::async_context_value`, and JavaScript with
//! `probe.value()`.

use std::cell::Cell;
use std::panic::AssertUnwindSafe;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::AsyncContextFrame;
use jsg::FromJS;
use jsg::ToJS;
use jsg::TracedAsyncContextFrame;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::Harness;

/// The value the current frame stores, or `None` in the root frame.
fn current(lock: &mut jsg::Lock) -> Option<String> {
    Harness::async_context_value(lock).map(|value| String::from_js(lock, value).unwrap())
}

/// Captures the frame of a new `run_in_async_context` that stores `value`.
fn frame_storing(lock: &mut jsg::Lock, value: &str) -> AsyncContextFrame {
    let value = value.to_js(lock).unwrap();
    Harness::run_in_async_context(lock, value, AsyncContextFrame::current)
}

/// Lets JavaScript read the current frame, and holds a frame for later calls.
#[jsg_resource]
struct Probe {
    frame: Cell<Option<TracedAsyncContextFrame>>,
    /// Whether the frame current before `run_in_frame` entered its frame was current again
    /// after it.
    restored: Cell<bool>,
    /// Whether the callback `run_in_frame` called failed with a pending error.
    saw_pending: Cell<bool>,
}

impl Probe {
    /// A strong handle to the captured frame. The traced frame goes back into its field before
    /// anything can run JavaScript, which the caller may do with the handle it gets.
    fn frame(&self, lock: &mut jsg::Lock) -> AsyncContextFrame {
        let traced = self.frame.take().unwrap();
        let frame = traced.to_async_context_frame(lock);
        self.frame.set(Some(traced));
        frame
    }
}

#[jsg_resource]
impl Probe {
    /// The value the current frame stores, or `undefined` in the root frame.
    #[jsg_method]
    fn value<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        Harness::async_context_value(lock).unwrap_or_else(|| lock.undefined())
    }

    /// Captures the frame of a new `run_in_async_context` that stores `value`. Nothing but
    /// this resource holds the frame afterwards.
    #[jsg_method]
    fn capture_in<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        let frame = Harness::run_in_async_context(lock, value, TracedAsyncContextFrame::current);
        self.frame.set(Some(frame));
    }

    /// The value the captured frame stores, read in that frame.
    #[jsg_method]
    fn value_in_frame<'s>(&self, lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        self.frame(lock).enter(lock, |lock| self.value(lock))
    }

    /// Calls `callback` in the captured frame.
    #[jsg_method]
    fn run_in_frame<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        callback: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let callback = v8::Local::<v8::Function>::try_from(callback).unwrap();
        let frame = self.frame(lock);
        // Compared as handles, since converting them could run JavaScript, which must not run
        // while execution is terminating.
        let before = Harness::async_context_value(lock);
        let result = frame.enter(lock, |lock| callback.call(lock, None, &[]));
        self.restored
            .set(Harness::async_context_value(lock) == before);
        self.saw_pending
            .set(result.as_ref().is_err_and(jsg::Error::is_pending));
        result.map(|_| ())
    }

    /// Requests termination, which takes effect when JavaScript next checks for interrupts.
    #[jsg_method]
    fn terminate(&self, lock: &mut jsg::Lock) {
        lock.terminate_execution();
    }
}

fn install_probe(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>) -> jsg::Rc<Probe> {
    let probe = jsg::Rc::new(Probe {
        frame: Cell::new(None),
        restored: Cell::new(false),
        saw_pending: Cell::new(false),
    });
    ctx.set_global(lock, "probe", probe.clone());
    probe
}

#[test]
fn captured_frame_is_entered_later() {
    Harness::new().run_in_context(|lock, ctx| {
        install_probe(lock, ctx);
        let frame = frame_storing(lock, "a");
        assert!(!frame.is_root());
        assert_eq!(current(lock), None);
        let seen = frame.enter(lock, |lock| {
            assert_eq!(current(lock).as_deref(), Some("a"));
            ctx.eval::<String>(lock, "probe.value()").unwrap()
        });
        assert_eq!(seen, "a");
        assert_eq!(current(lock), None);
        Ok(())
    });
}

#[test]
fn frame_captured_in_root_is_root() {
    Harness::new().run_in_context(|lock, _ctx| {
        let frame = AsyncContextFrame::current(lock);
        assert!(frame.is_root());
        assert!(AsyncContextFrame::root().is_root());
        assert!(TracedAsyncContextFrame::current(lock).is_root());
        frame.enter(lock, |lock| assert_eq!(current(lock), None));
        Ok(())
    });
}

/// Rust treats any embedder data but `undefined` as a frame, while C++ recognizes a frame by its
/// wrapper. Since Rust sets only `undefined` and values it read, the two agree on whether the
/// current frame is the root in every state Rust can leave the isolate in.
#[test]
fn rust_and_cpp_agree_on_root_frame() {
    /// Asserts that Rust and C++ agree on whether the current frame is the root, and returns it.
    fn is_root(lock: &mut jsg::Lock<'_>) -> bool {
        let rust = AsyncContextFrame::current(lock).is_root();
        assert_eq!(rust, Harness::cpp_sees_root_frame(lock));
        rust
    }

    Harness::new().run_in_context(|lock, _ctx| {
        assert!(is_root(lock));
        let frame = frame_storing(lock, "a");
        frame.enter(lock, |lock| assert!(!is_root(lock)));
        assert!(is_root(lock));

        let value = "outer".to_js(lock)?;
        Harness::run_in_async_context(lock, value, |lock| {
            assert!(!is_root(lock));
            AsyncContextFrame::root().enter(lock, |lock| assert!(is_root(lock)));
            TracedAsyncContextFrame::root().enter(lock, |lock| assert!(is_root(lock)));
            frame.enter(lock, |lock| assert!(!is_root(lock)));
            assert!(!is_root(lock));
        });
        assert!(is_root(lock));
        Ok(())
    });
}

#[test]
fn entering_root_frame_leaves_frame_for_its_duration() {
    Harness::new().run_in_context(|lock, _ctx| {
        let value = "outer".to_js(lock)?;
        Harness::run_in_async_context(lock, value, |lock| {
            AsyncContextFrame::root().enter(lock, |lock| assert_eq!(current(lock), None));
            TracedAsyncContextFrame::root().enter(lock, |lock| assert_eq!(current(lock), None));
            assert_eq!(current(lock).as_deref(), Some("outer"));
        });
        Ok(())
    });
}

#[test]
fn nested_enters_restore_each_previous_frame() {
    Harness::new().run_in_context(|lock, _ctx| {
        let a = frame_storing(lock, "a");
        let b: TracedAsyncContextFrame = frame_storing(lock, "b").into();
        a.enter(lock, |lock| {
            b.enter(lock, |lock| {
                assert_eq!(current(lock).as_deref(), Some("b"));
                a.enter(lock, |lock| assert_eq!(current(lock).as_deref(), Some("a")));
                assert_eq!(current(lock).as_deref(), Some("b"));
            });
            assert_eq!(current(lock).as_deref(), Some("a"));
        });
        assert_eq!(current(lock), None);
        Ok(())
    });
}

#[test]
fn clone_enters_same_frame() {
    Harness::new().run_in_context(|lock, _ctx| {
        let a = frame_storing(lock, "a");
        let clone = a.clone(lock);
        drop(a);
        clone.enter(lock, |lock| assert_eq!(current(lock).as_deref(), Some("a")));
        let traced = TracedAsyncContextFrame::from(clone);
        let traced_clone = traced.clone(lock);
        drop(traced);
        traced_clone.enter(lock, |lock| assert_eq!(current(lock).as_deref(), Some("a")));
        Ok(())
    });
}

#[test]
fn enter_returns_closure_result_and_restores_after_error() {
    Harness::new().run_in_context(|lock, _ctx| {
        let a = frame_storing(lock, "a");
        let value = "outer".to_js(lock)?;
        Harness::run_in_async_context(lock, value, |lock| {
            let result: Result<(), jsg::Error> =
                a.enter(lock, |_lock| Err(jsg::Error::new_type_error("failed")));
            assert_eq!(result.unwrap_err().message, "failed");
            assert_eq!(current(lock).as_deref(), Some("outer"));
            let handle = a.enter(lock, |lock| "from a".to_js(lock)).unwrap();
            assert_eq!(String::from_js(lock, handle).unwrap(), "from a");
        });
        Ok(())
    });
}

#[test]
fn enter_restores_after_panic() {
    Harness::new().run_in_context(|lock, _ctx| {
        let a = frame_storing(lock, "a");
        let value = "outer".to_js(lock)?;
        Harness::run_in_async_context(lock, value, |lock| {
            let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
                a.enter(lock, |_lock| panic!("panic in frame"));
            }));
            assert!(result.is_err());
            assert_eq!(current(lock).as_deref(), Some("outer"));
        });
        Ok(())
    });
}

/// A callback that terminates execution fails, and the frame current before `enter` is
/// current again while the termination still propagates.
#[test]
fn enter_restores_while_terminating() {
    Harness::new().run_in_context(|lock, ctx| {
        let probe = install_probe(lock, ctx);
        let value = "a".to_js(lock)?;
        probe.capture_in(lock, value);
        let value = "outer".to_js(lock)?;
        let result = Harness::run_in_async_context(lock, value, |lock| {
            let result = ctx.eval_raw(
                lock,
                "probe.runInFrame(() => { probe.terminate(); while (true) {} }); 'not terminated'",
            );
            assert_eq!(current(lock).as_deref(), Some("outer"));
            result.is_err()
        });
        assert!(result, "the script was not terminated");
        assert!(probe.saw_pending.get());
        assert!(probe.restored.get());
        assert_eq!(current(lock), None);
        Ok(())
    });
}

/// V8 carries the current frame into the promise reactions registered in it, by JavaScript or
/// by Rust, which run in that frame after `enter` has returned.
#[test]
fn promise_reactions_registered_in_entered_frame_run_in_it() {
    Harness::new().run_in_context(|lock, ctx| {
        install_probe(lock, ctx);
        let a = frame_storing(lock, "a");
        let resolver = v8::PromiseResolver::new(lock)?;
        let promise = resolver.promise();
        ctx.set_global(lock, "pending", v8::Local::<v8::Value>::from(promise));
        let in_root = promise.then(lock, (), |lock, (), _value| {
            Ok(Harness::async_context_value(lock).unwrap_or_else(|| lock.undefined()))
        })?;
        let in_a = a.enter(lock, |lock| {
            ctx.eval_raw(
                lock,
                "globalThis.seen = []; pending.then(() => seen.push(probe.value()))",
            )
            .unwrap();
            promise.then(lock, (), |lock, (), _value| {
                Ok(Harness::async_context_value(lock).unwrap_or_else(|| lock.undefined()))
            })
        })?;
        let undefined = lock.undefined();
        resolver.resolve(lock, undefined)?;
        Harness::run_microtasks(lock);

        assert_eq!(ctx.eval::<String>(lock, "seen.join()").unwrap(), "a");
        let result = in_a.result(lock).unwrap();
        assert_eq!(String::from_js(lock, result)?, "a");
        assert!(in_root.result(lock).unwrap().is_undefined());
        assert_eq!(current(lock), None);
        Ok(())
    });
}

/// A frame captured under one lock is entered under a later one, as code that resumes after
/// async I/O does.
#[test]
fn frame_is_entered_in_later_lock() {
    let harness = Harness::new();
    let mut kept = None;
    harness.run_in_context(|lock, _ctx| {
        kept = Some(frame_storing(lock, "a"));
        Ok(())
    });
    let frame = kept.unwrap();
    harness.run_in_context(|lock, _ctx| {
        // Only the `AsyncContextFrame` holds the frame's wrapper now. A minor GC would drop a
        // wrapper that nothing else reaches, and the C++ frame with it.
        Harness::request_minor_gc(lock);
        Harness::request_gc(lock);
        assert_eq!(current(lock), None);
        frame.enter(lock, |lock| assert_eq!(current(lock).as_deref(), Some("a")));
        Ok(())
    });
}

static STORED_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A value to store in a frame, which counts its drops.
#[jsg_resource]
struct Stored {
    /// The probe whose frame stores this value, which closes a cycle through the frame.
    owner: Cell<Option<jsg::Rc<Probe>>>,
}

impl Drop for Stored {
    fn drop(&mut self) {
        STORED_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Stored {}

/// A frame held only by a resource keeps itself and the values it stores alive across GC, and
/// GC collects a cycle through it: the probe holds the frame, which stores a value that holds
/// the probe.
///
/// Each step runs under its own lock, so that no handle of an earlier step's scope keeps
/// anything alive.
#[test]
fn frame_in_resource_is_traced() {
    STORED_DROPS.store(0, Ordering::SeqCst);
    let harness = Harness::new();
    let mut kept = None;
    harness.run_in_context(|lock, _ctx| {
        let probe = jsg::Rc::new(Probe {
            frame: Cell::new(None),
            restored: Cell::new(false),
            saw_pending: Cell::new(false),
        });
        // The probe's wrapper is reachable, but the probe has no strong reference from Rust, so
        // GC traces its frame through the wrapper.
        let wrapper = probe.clone().to_js(lock)?;
        let stored = jsg::Rc::new(Stored {
            owner: Cell::new(Some(probe.clone())),
        })
        .to_js(lock)?;
        probe.capture_in(lock, stored);
        kept = Some((wrapper.to_global(lock), probe.downgrade()));
        Ok(())
    });
    let (wrapper, probe) = kept.unwrap();

    // Each step runs two GCs, so that the result does not depend on whether a visit during the
    // first one is what turns a strong handle into a traced one.
    harness.run_in_context(|lock, _ctx| {
        Harness::request_gc(lock);
        Harness::request_gc(lock);
        assert_eq!(
            STORED_DROPS.load(Ordering::SeqCst),
            0,
            "the stored value was collected while the frame was held"
        );
        let probe = probe.upgrade().unwrap();
        let stored = probe.value_in_frame(lock);
        assert!(jsg::Rc::<Stored>::from_js(lock, stored).is_ok());
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        drop(wrapper);
        Harness::request_gc(lock);
        Harness::request_gc(lock);
        assert_eq!(
            STORED_DROPS.load(Ordering::SeqCst),
            1,
            "the cycle through the frame was not collected"
        );
        Ok(())
    });
}

static MARKER_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A value to store in a frame, which counts its drops.
#[jsg_resource]
struct Marker;

impl Drop for Marker {
    fn drop(&mut self) {
        MARKER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Marker {}

/// A frame in the data of a `v8::Function::new` function, which enters it to call its argument,
/// like C++ JSG's `AsyncContextFrame::wrap()`, survives GC while the function is reachable, and
/// is released with it.
///
/// Each step runs under its own lock, and sets no globals, so that only the function keeps the
/// frame alive across the GCs between them.
#[test]
fn frame_in_function_data_lives_as_long_as_function() {
    MARKER_DROPS.store(0, Ordering::SeqCst);
    let harness = Harness::new();
    let mut kept = None;
    harness.run_in_context(|lock, _ctx| {
        let marker = jsg::Rc::new(Marker).to_js(lock)?;
        let frame = Harness::run_in_async_context(lock, marker, TracedAsyncContextFrame::current);
        let run_in_frame = v8::Function::new(lock, frame, |lock, frame, args| {
            let callback = v8::Local::<v8::Function>::try_from(args.get(0)).unwrap();
            frame.enter(lock, |lock| callback.call(lock, None, &[]))
        })?;
        kept = Some(run_in_frame.to_global(lock));
        Ok(())
    });
    let run_in_frame = kept.unwrap();

    harness.run_in_context(|lock, _ctx| {
        Harness::request_gc(lock);
        Harness::request_gc(lock);
        assert_eq!(MARKER_DROPS.load(Ordering::SeqCst), 0);
        let read_value = v8::Function::new(lock, (), |lock, (), _args| {
            Ok(Harness::async_context_value(lock).unwrap_or_else(|| lock.undefined()))
        })?;
        let value = run_in_frame
            .as_local(lock)
            .call(lock, None, &[read_value.into()])?;
        assert!(jsg::Rc::<Marker>::from_js(lock, value).is_ok());
        assert_eq!(current(lock), None);
        Ok(())
    });

    harness.run_in_context(|lock, _ctx| {
        drop(run_in_frame);
        Harness::request_gc(lock);
        Harness::request_gc(lock);
        assert_eq!(
            MARKER_DROPS.load(Ordering::SeqCst),
            1,
            "the frame outlived the function that held it"
        );
        Ok(())
    });
}

/// A frame converted from a resource's traced frame holds the frame strongly on its own.
#[test]
fn traced_frame_converts_to_async_context_frame() {
    let harness = Harness::new();
    let mut kept = None;
    harness.run_in_context(|lock, _ctx| {
        let traced: TracedAsyncContextFrame = frame_storing(lock, "a").into();
        kept = Some(traced.to_async_context_frame(lock));
        assert!(
            TracedAsyncContextFrame::root()
                .to_async_context_frame(lock)
                .is_root()
        );
        Ok(())
    });
    let frame = kept.unwrap();
    harness.run_in_context(|lock, _ctx| {
        Harness::request_minor_gc(lock);
        Harness::request_gc(lock);
        frame.enter(lock, |lock| assert_eq!(current(lock).as_deref(), Some("a")));
        Ok(())
    });
}

/// Frames may outlive their isolate: one held by a resource at teardown, and one held by Rust
/// and dropped after the isolate is disposed.
#[test]
fn frames_dropped_at_or_after_teardown() {
    let harness = Harness::new();
    let mut kept = None;
    harness.run_in_context(|lock, ctx| {
        let probe = install_probe(lock, ctx);
        let value = "held by the probe".to_js(lock)?;
        probe.capture_in(lock, value);
        kept = Some(frame_storing(lock, "held by Rust"));
        Ok(())
    });
    drop(harness);
    drop(kept.unwrap());
}
