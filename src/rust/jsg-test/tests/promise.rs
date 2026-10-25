// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JavaScript promises in Rust: creating and settling them through `v8::PromiseResolver`, and
//! reacting to them with `Local<Promise>::then`, `catch` and `then_or_catch`.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg::v8::PromiseState;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Evaluates `code`, which records what happens in the global array `log`, runs the microtasks,
/// and returns the log joined with `|`.
fn log_after_microtasks(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    ctx.eval_raw(lock, &format!("globalThis.log = []; {code}"))
        .unwrap();
    crate::Harness::run_microtasks(lock);
    ctx.eval(lock, "log.join('|')").unwrap()
}

/// What a promise settled with, as `"fulfilled: <value>"` or `"rejected: <name>: <message>"`.
const RECORD: &str = "globalThis.record = (p) => p.then(
    (v) => log.push(`fulfilled: ${v}`),
    (e) => log.push(`rejected: ${e?.name}: ${e?.message ?? e}`));";

// =============================================================================
// Resolvers and promise state
// =============================================================================

#[test]
fn resolver_settles_its_promise_once() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let resolver = v8::PromiseResolver::new(lock).unwrap();
        let promise = resolver.promise();
        assert_eq!(promise.state(), PromiseState::Pending);
        assert!(promise.result(lock).is_none());

        let value = jsg::Number::new(42.0).to_js(lock);
        resolver.resolve(lock, value)?;
        let other = "ignored".to_js(lock);
        // Settling a settled promise does nothing.
        resolver.reject(lock, other)?;

        assert_eq!(promise.state(), PromiseState::Fulfilled);
        let result = promise.result(lock).unwrap();
        #[expect(clippy::float_cmp, reason = "42 is exactly representable")]
        {
            assert_eq!(jsg::Number::from_js(lock, result)?.value(), 42.0);
        }
        Ok(())
    });
}

#[test]
fn rejected_promise_holds_its_reason() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let reason = lock.range_error("too far");
        let promise = v8::Promise::rejected(lock, reason.into()).unwrap();
        promise.mark_as_handled();
        assert_eq!(promise.state(), PromiseState::Rejected);
        assert_eq!(promise.result(lock).unwrap(), reason.into());
        Ok(())
    });
}

#[test]
fn resolved_returns_promises_unchanged_and_follows_thenables() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let existing = ctx
            .eval_raw(lock, "globalThis.p = Promise.resolve(1); p")
            .unwrap();
        let same = v8::Promise::resolved(lock, existing).unwrap();
        assert_eq!(v8::Local::<v8::Value>::from(same), existing);

        // Unlike `Promise.resolve`, which would wrap a subclass instance in a new promise.
        let subclass = ctx
            .eval_raw(
                lock,
                "class PromiseSubclass extends Promise {}; new PromiseSubclass(() => {})",
            )
            .unwrap();
        let same_subclass = v8::Promise::resolved(lock, subclass).unwrap();
        assert_eq!(v8::Local::<v8::Value>::from(same_subclass), subclass);

        let thenable = ctx
            .eval_raw(lock, "({ then(resolve) { resolve('from thenable'); } })")
            .unwrap();
        let followed = v8::Promise::resolved(lock, thenable).unwrap();
        ctx.set_global(lock, "followed", followed);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(followed)")),
            "fulfilled: from thenable"
        );
        Ok(())
    });
}

#[test]
fn rejecting_with_an_error_creates_its_exception() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let err = jsg::Error::new_type_error("bad input");
        let promise = v8::Promise::rejected_with_error(lock, &err).unwrap();
        ctx.set_global(lock, "p", promise);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(p)")),
            "rejected: TypeError: bad input"
        );
        Ok(())
    });
}

#[test]
fn promise_argument_accepts_any_value() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let value = "plain".to_js(lock);
        let promise = v8::Local::<v8::Promise>::from_js(lock, value)?;
        ctx.set_global(lock, "p", promise);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(p)")),
            "fulfilled: plain"
        );
        Ok(())
    });
}

// =============================================================================
// then
// =============================================================================

/// Registering a reaction runs no JavaScript: neither the promise's `then` nor its `constructor`
/// (for the species) is read, so getters that throw don't stop any of the three.
#[test]
fn promise_reactions_bypass_user_properties() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = ctx
            .eval_raw(
                lock,
                "const p = Promise.resolve('kept');
                 Object.defineProperties(p, {
                   then: { get() { throw new Error('then read'); } },
                   constructor: { get() { throw new Error('constructor read'); } },
                 });
                 p",
            )
            .unwrap();
        let promise = v8::Local::<v8::Promise>::try_from(promise).unwrap();
        let fulfilled = promise
            .then(lock, (), |_lock, (), value| Ok(value))
            .unwrap();
        let caught = promise
            .catch(lock, (), |lock, (), _reason| Ok(lock.undefined()))
            .unwrap();
        let either = promise
            .then_or_catch(
                lock,
                (),
                |_lock, (), value| Ok(value),
                |lock, (), _reason| Ok(lock.undefined()),
            )
            .unwrap();
        crate::Harness::run_microtasks(lock);
        for reaction in [fulfilled, caught, either] {
            assert_eq!(reaction.state(), PromiseState::Fulfilled);
            let result = reaction.result(lock).unwrap();
            assert_eq!(String::from_js(lock, result)?, "kept");
        }
        Ok(())
    });
}

#[test]
fn then_runs_callbacks_as_microtasks_with_their_data() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = ctx.eval_raw(lock, "Promise.resolve('world')").unwrap();
        let promise = v8::Local::<v8::Promise>::try_from(promise).unwrap();
        let greeted = promise
            .then(lock, "hello".to_owned(), |lock, greeting, value| {
                let name = String::from_js(lock, value)?;
                Ok(format!("{greeting}, {name}").to_js(lock))
            })
            .unwrap();
        // Reactions never run synchronously.
        assert_eq!(greeted.state(), PromiseState::Pending);
        crate::Harness::run_microtasks(lock);
        let result = greeted.result(lock).unwrap();
        assert_eq!(String::from_js(lock, result)?, "hello, world");
        Ok(())
    });
}

#[test]
fn then_rejects_with_callback_error_and_handles_rejections() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let fulfilled = ctx.eval_raw(lock, "Promise.resolve(1)").unwrap();
        let fulfilled = v8::Local::<v8::Promise>::try_from(fulfilled).unwrap();
        let failed = fulfilled
            .then(lock, (), |_lock, (), _value| {
                Err(jsg::Error::new_range_error("out of range"))
            })
            .unwrap();
        ctx.set_global(lock, "failed", failed);

        let rejected = ctx
            .eval_raw(lock, "Promise.reject(new TypeError('nope'))")
            .unwrap();
        let rejected = v8::Local::<v8::Promise>::try_from(rejected).unwrap();
        let recovered = rejected
            .catch(lock, (), |lock, (), reason| {
                let err = jsg::Error::from_js(lock, reason)?;
                Ok(format!("recovered from {}", err.message).to_js(lock))
            })
            .unwrap();
        ctx.set_global(lock, "recovered", recovered);

        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(failed).then(() => record(recovered))")
            ),
            "rejected: RangeError: out of range|fulfilled: recovered from nope"
        );
        Ok(())
    });
}

/// `then` passes a rejection through and `catch` a fulfillment, natively: the value arrives
/// unchanged, and no intermediate promise is rejected without a handler.
#[test]
fn then_and_catch_pass_the_other_outcome_through() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let rejected = ctx
            .eval_raw(
                lock,
                "globalThis.error = new TypeError('nope'); Promise.reject(error)",
            )
            .unwrap();
        let rejected = v8::Local::<v8::Promise>::try_from(rejected).unwrap();
        let skipped = rejected
            .then(lock, (), |_lock, (), _value| {
                Err(jsg::Error::new_type_error("on_fulfilled ran"))
            })
            .unwrap();
        ctx.set_global(lock, "skipped", skipped);

        let fulfilled = ctx.eval_raw(lock, "Promise.resolve('kept')").unwrap();
        let fulfilled = v8::Local::<v8::Promise>::try_from(fulfilled).unwrap();
        let kept = fulfilled
            .catch(lock, (), |_lock, (), _reason| {
                Err(jsg::Error::new_type_error("on_rejected ran"))
            })
            .unwrap();
        ctx.set_global(lock, "kept", kept);

        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} skipped.catch((e) => log.push(`same error: ${{e === error}}`));
                    record(kept);"
                )
            ),
            "same error: true|fulfilled: kept"
        );
        Ok(())
    });
}

/// Reacts to the promise that `code` evaluates to with both callbacks of `then_or_catch`.
fn react_to_outcome<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    code: &str,
) -> v8::Local<'s, v8::Promise> {
    let promise = ctx.eval_raw(lock, code).unwrap();
    v8::Local::<v8::Promise>::try_from(promise)
        .unwrap()
        .then_or_catch(
            lock,
            "outcome".to_owned(),
            |lock, what, value| {
                let value = String::from_js(lock, value)?;
                Ok(format!("{what}: fulfilled with {value}").to_js(lock))
            },
            |lock, what, reason| {
                let err = jsg::Error::from_js(lock, reason)?;
                Ok(format!("{what}: rejected with {}", err.message).to_js(lock))
            },
        )
        .unwrap()
}

#[test]
fn then_or_catch_runs_the_callback_for_the_outcome() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let fulfilled = react_to_outcome(lock, ctx, "Promise.resolve('yes')");
        let rejected = react_to_outcome(lock, ctx, "Promise.reject(new Error('no'))");
        crate::Harness::run_microtasks(lock);
        let result = fulfilled.result(lock).unwrap();
        assert_eq!(
            String::from_js(lock, result)?,
            "outcome: fulfilled with yes"
        );
        let result = rejected.result(lock).unwrap();
        assert_eq!(String::from_js(lock, result)?, "outcome: rejected with no");
        Ok(())
    });
}

#[test]
fn panic_in_then_callback_is_an_internal_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = ctx.eval_raw(lock, "Promise.resolve(1)").unwrap();
        let promise = v8::Local::<v8::Promise>::try_from(promise).unwrap();
        let panicked = promise
            .then(
                lock,
                (),
                |_lock, (), _value| -> Result<v8::Local<v8::Value>, jsg::Error> {
                    panic!("intentional panic in a then callback");
                },
            )
            .unwrap();
        panicked.mark_as_handled();
        crate::Harness::run_microtasks(lock);
        assert_eq!(panicked.state(), PromiseState::Rejected);
        let reason = panicked.result(lock).unwrap();
        let err = jsg::Error::from_js(lock, reason)?;
        assert!(err.message.contains("internal error"), "{}", err.message);
        Ok(())
    });
}

/// Counts its drops in the counter it points to.
struct DropCounter(&'static AtomicUsize);

impl Drop for DropCounter {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

impl jsg::Traced for DropCounter {}

static SETTLED_DATA_DROPS: AtomicUsize = AtomicUsize::new(0);

#[test]
fn then_data_is_dropped_once_the_promise_settles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resolver = v8::PromiseResolver::new(lock).unwrap();
        resolver
            .promise()
            .then_or_catch(
                lock,
                DropCounter(&SETTLED_DATA_DROPS),
                |lock, _, _| Ok(lock.undefined()),
                |lock, _, _| Ok(lock.undefined()),
            )
            .unwrap();
        // The pending promise keeps its reactions, and with them the data, which they share.
        ctx.set_global(lock, "resolver", resolver.promise());
        crate::Harness::request_gc(lock);
        assert_eq!(SETTLED_DATA_DROPS.load(Ordering::SeqCst), 0);
        let value = lock.undefined();
        resolver.resolve(lock, value)?;
        crate::Harness::run_microtasks(lock);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SETTLED_DATA_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

static SHARED_LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

#[jsg_resource]
struct Leaf {
    _drops: DropCounter,
}

#[jsg_resource]
impl Leaf {}

/// Both reactions of `then_or_catch` trace a `jsg::Rc` in their data through one owner. Two
/// owners of one `jsg::Rc` would fail C++'s single-parent assertion as soon as the second
/// reaction was created, and again whenever the garbage collector traced them.
#[test]
fn then_or_catch_data_holds_a_resource() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let leaf = jsg::Rc::new(Leaf {
            _drops: DropCounter(&SHARED_LEAF_DROPS),
        });
        let resolver = v8::PromiseResolver::new(lock).unwrap();
        let settled = resolver
            .promise()
            .then_or_catch(
                lock,
                leaf,
                |lock, leaf, _| Ok(leaf.clone().to_js(lock)),
                |lock, _, _| Ok(lock.undefined()),
            )
            .unwrap();
        ctx.set_global(lock, "resolver", resolver.promise());
        crate::Harness::request_gc(lock);
        assert_eq!(SHARED_LEAF_DROPS.load(Ordering::SeqCst), 0);
        let value = lock.undefined();
        resolver.resolve(lock, value)?;
        crate::Harness::run_microtasks(lock);
        // The reaction returned the resource, which the settled promise now holds.
        let result = settled.result(lock).unwrap();
        assert!(jsg::Rc::<Leaf>::from_js(lock, result).is_ok());
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(SHARED_LEAF_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

static TEARDOWN_DATA_DROPS: AtomicUsize = AtomicUsize::new(0);
static TEARDOWN_LEAF_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Reaction data that holds a resource and a handle back to the promise it reacts to.
struct TeardownData {
    _drops: DropCounter,
    leaf: jsg::Rc<Leaf>,
    promise: v8::TracedGlobal<v8::Promise>,
}

impl jsg::Traced for TeardownData {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.leaf.trace(visitor);
        self.promise.trace(visitor);
    }
}

/// The data of the reactions to a promise that is still pending, and reachable, when the
/// isolate is torn down is dropped by the teardown, with what it owns.
#[test]
fn reaction_data_of_pending_promise_is_dropped_with_its_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resolver = v8::PromiseResolver::new(lock).unwrap();
        let promise = resolver.promise();
        let data = TeardownData {
            _drops: DropCounter(&TEARDOWN_DATA_DROPS),
            leaf: jsg::Rc::new(Leaf {
                _drops: DropCounter(&TEARDOWN_LEAF_DROPS),
            }),
            promise: promise.to_traced_global(lock),
        };
        promise
            .then_or_catch(
                lock,
                data,
                |lock, _, _| Ok(lock.undefined()),
                |lock, _, _| Ok(lock.undefined()),
            )
            .unwrap();
        ctx.set_global(lock, "pending", promise);
        crate::Harness::request_gc(lock);
        assert_eq!(TEARDOWN_DATA_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(TEARDOWN_LEAF_DROPS.load(Ordering::SeqCst), 0);
        Ok(())
    });
    drop(harness);
    assert_eq!(TEARDOWN_DATA_DROPS.load(Ordering::SeqCst), 1);
    assert_eq!(TEARDOWN_LEAF_DROPS.load(Ordering::SeqCst), 1);
}

/// What a `Watcher` has seen, shared by the watcher and the reactions it registers.
#[jsg_resource]
struct WatcherState {
    seen: RefCell<Vec<String>>,
}

#[jsg_resource]
impl WatcherState {}

/// Calls a JavaScript callback with what each watched promise fulfills with.
#[jsg_resource]
struct Watcher {
    state: jsg::Rc<WatcherState>,
}

/// The data of a `Watcher`'s reaction: the watcher's state, through its own `jsg::Rc`, and the
/// callback.
struct Watch {
    state: jsg::Rc<WatcherState>,
    callback: v8::TracedGlobal<v8::Function>,
}

impl jsg::Traced for Watch {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.state.trace(visitor);
        self.callback.trace(visitor);
    }
}

#[jsg_resource]
impl Watcher {
    #[jsg_method]
    fn watch<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: v8::Local<'s, v8::Promise>,
        callback: v8::Local<'s, v8::Value>,
    ) -> Result<v8::Local<'s, v8::Promise>, jsg::Error> {
        let callback = v8::Local::<v8::Function>::try_from(callback)
            .map_err(|_| jsg::Error::new_type_error("callback must be a function"))?;
        let watch = Watch {
            state: self.state.clone(),
            callback: callback.to_traced_global(lock),
        };
        promise.then(lock, watch, |lock, watch, value| {
            let seen = String::from_js(lock, value)?;
            watch.state.seen.borrow_mut().push(seen);
            // No borrow is held across the call, whose JavaScript calls back into the watcher.
            let callback = watch.callback.as_local(lock);
            callback.call::<v8::Local<v8::Value>>(lock, None, &[value])
        })
    }

    #[jsg_method]
    fn seen(&self) -> String {
        self.state.seen.borrow().join(",")
    }
}

#[test]
fn reaction_calls_back_into_the_resource_that_registered_it() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let watcher = jsg::Rc::new(Watcher {
            state: jsg::Rc::new(WatcherState {
                seen: RefCell::new(Vec::new()),
            }),
        });
        ctx.set_global(lock, "watcher", watcher);
        let code = format!(
            "{RECORD}
            watcher.watch(Promise.resolve('a'), (v) => {{
                log.push(`got ${{v}}, seen ${{watcher.seen()}}`);
                record(watcher.watch(Promise.resolve('b'), (v) => {{
                    log.push(`got ${{v}}, seen ${{watcher.seen()}}`);
                    return 'done';
                }}));
            }});"
        );
        assert_eq!(
            log_after_microtasks(lock, ctx, &code),
            "got a, seen a|got b, seen a,b|fulfilled: done"
        );
        Ok(())
    });
}

/// Terminates execution, then tries each promise operation.
#[jsg_resource]
struct TerminatingPromises {
    /// Each operation, in order, and whether it failed with a pending error.
    pending: RefCell<Vec<String>>,
    /// Whether the promise that was settled while terminating stayed pending.
    still_pending: Cell<bool>,
}

#[jsg_resource]
impl TerminatingPromises {
    /// Requests a termination, which V8 delivers while `spin` runs, then uses promises.
    #[jsg_method]
    fn run<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        spin: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let resolver = v8::PromiseResolver::new(lock)?;
        let promise = resolver.promise();
        lock.terminate_execution();
        let spin = v8::Local::<v8::Function>::try_from(spin).unwrap();
        let err = spin
            .call::<v8::Local<v8::Value>>(lock, None, &[])
            .unwrap_err();
        assert!(err.is_pending());

        let mut pending = Vec::new();
        let mut record = |what: &'static str, result: Result<(), jsg::Error>| {
            pending.push(format!(
                "{what}: {}",
                result.is_err_and(|err| err.is_pending())
            ));
        };
        record(
            "PromiseResolver::new",
            v8::PromiseResolver::new(lock).map(|_| ()),
        );
        record(
            "then",
            promise.then(lock, (), |_, (), value| Ok(value)).map(|_| ()),
        );
        record(
            "catch",
            promise
                .catch(lock, (), |_, (), reason| Ok(reason))
                .map(|_| ()),
        );
        record(
            "then_or_catch",
            promise
                .then_or_catch(
                    lock,
                    (),
                    |_, (), value| Ok(value),
                    |_, (), reason| Ok(reason),
                )
                .map(|_| ()),
        );
        let value = lock.undefined();
        record("resolve", resolver.resolve(lock, value));
        record("reject", resolver.reject(lock, value));
        record("reject_with_error", resolver.reject_with_error(lock, &err));
        // An error that isn't pending: creating its exception while execution is terminating
        // fails a V8 debug check, so none of these creates it.
        let type_error = jsg::Error::new_type_error("rejected while terminating");
        record(
            "exception(TypeError)",
            lock.exception(&type_error).map(|_| ()),
        );
        record(
            "reject_with_error(TypeError)",
            resolver.reject_with_error(lock, &type_error),
        );
        record(
            "Promise::rejected_with_error(TypeError)",
            v8::Promise::rejected_with_error(lock, &type_error).map(|_| ()),
        );
        self.still_pending
            .set(promise.state() == PromiseState::Pending);
        *self.pending.borrow_mut() = pending;
        Err(err)
    }
}

/// While execution is terminating, every promise operation creates and settles nothing and
/// returns a pending error, so that returning it keeps the termination. Entering V8 would clear
/// the termination, and JavaScript would run on. Neither does `Lock::exception` create the
/// exception for an error.
#[test]
fn promise_operations_while_terminating_keep_the_termination() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promises = jsg::Rc::new(TerminatingPromises {
            pending: RefCell::new(Vec::new()),
            still_pending: Cell::new(false),
        });
        ctx.set_global(lock, "promises", promises.clone());
        let result = ctx.eval_raw(
            lock,
            "globalThis.caught = false; globalThis.after = false;
             try { promises.run(() => { while (true) {} }); }
             catch (e) { globalThis.caught = true; }
             globalThis.after = true;",
        );
        assert!(result.is_err(), "the script was not terminated");
        assert_eq!(
            *promises.pending.borrow(),
            [
                "PromiseResolver::new: true",
                "then: true",
                "catch: true",
                "then_or_catch: true",
                "resolve: true",
                "reject: true",
                "reject_with_error: true",
                "exception(TypeError): true",
                "reject_with_error(TypeError): true",
                "Promise::rejected_with_error(TypeError): true",
            ]
        );
        assert!(promises.still_pending.get());
        assert!(!ctx.eval::<bool>(lock, "globalThis.caught").unwrap());
        assert!(!ctx.eval::<bool>(lock, "globalThis.after").unwrap());
        Ok(())
    });
}

// =============================================================================
// Consumer: a resource that settles a promise from a later call
// =============================================================================

/// Hands out a promise and settles it when JavaScript calls `resolve` or `fail` later, holding
/// the resolver in between, as an API waiting on an event would.
#[jsg_resource]
struct Deferred {
    resolver: RefCell<Option<v8::TracedGlobal<v8::PromiseResolver>>>,
}

#[jsg_resource]
impl Deferred {
    #[jsg_method]
    fn wait<'s>(&self, lock: &mut jsg::Lock<'s>) -> Result<v8::Local<'s, v8::Promise>, jsg::Error> {
        let resolver = v8::PromiseResolver::new(lock)?;
        *self.resolver.borrow_mut() = Some(resolver.to_traced_global(lock));
        Ok(resolver.promise())
    }

    #[jsg_method]
    fn resolve<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let resolver = self.take_resolver(lock)?;
        resolver.resolve(lock, value)
    }

    #[jsg_method]
    fn fail(&self, lock: &mut jsg::Lock<'_>, message: String) -> Result<(), jsg::Error> {
        let resolver = self.take_resolver(lock)?;
        resolver.reject_with_error(lock, &jsg::Error::new_range_error(message))
    }
}

impl Deferred {
    fn take_resolver<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> Result<v8::Local<'s, v8::PromiseResolver>, jsg::Error> {
        let resolver = self.resolver.borrow_mut().take();
        resolver
            .map(|resolver| resolver.as_local(lock))
            .ok_or_else(|| jsg::Error::new_type_error("nothing is waiting"))
    }
}

#[test]
fn resource_settles_promise_from_later_call() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let deferred = jsg::Rc::new(Deferred {
            resolver: RefCell::new(None),
        });
        ctx.set_global(lock, "deferred", deferred);
        let code = format!(
            "{RECORD}
            const first = deferred.wait();
            record(first);
            log.push(`pending: ${{first instanceof Promise}}`);
            deferred.resolve(42);
            record(deferred.wait());
            deferred.fail('too late');"
        );
        assert_eq!(
            log_after_microtasks(lock, ctx, &code),
            "pending: true|fulfilled: 42|rejected: RangeError: too late"
        );
        Ok(())
    });
}

static DEFERRED_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A promise that only its resolver's owner references.
#[jsg_resource]
struct DroppedDeferred {
    _drops: DropCounter,
    resolver: RefCell<Option<v8::TracedGlobal<v8::PromiseResolver>>>,
}

#[jsg_resource]
impl DroppedDeferred {
    #[jsg_method]
    fn wait<'s>(&self, lock: &mut jsg::Lock<'s>) -> Result<v8::Local<'s, v8::Promise>, jsg::Error> {
        let resolver = v8::PromiseResolver::new(lock)?;
        *self.resolver.borrow_mut() = Some(resolver.to_traced_global(lock));
        Ok(resolver.promise())
    }
}

#[test]
fn cycle_through_held_resolver_is_collected() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let deferred = jsg::Rc::new(DroppedDeferred {
            _drops: DropCounter(&DEFERRED_DROPS),
            resolver: RefCell::new(None),
        });
        ctx.set_global(lock, "deferred", deferred);
        // The promise's reaction holds the resource, which holds the resolver of the promise.
        ctx.eval_raw(
            lock,
            "const d = deferred; d.wait().then(() => d); delete globalThis.deferred",
        )
        .unwrap();
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(DEFERRED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

// =============================================================================
// Consumer: a read-through cache, shaped like MemoryCache.read(key, fallback)
// =============================================================================

/// The cache's entries and in-flight fallbacks. A resource of its own, so that the cache and the
/// reactions to in-flight fallbacks each hold a `jsg::Rc` to it and the garbage collector traces
/// its handles once, through it.
#[jsg_resource]
struct CacheStore {
    entries: RefCell<HashMap<String, v8::TracedGlobal<v8::Value>>>,
    in_flight: RefCell<HashMap<String, v8::TracedGlobal<v8::Promise>>>,
}

#[jsg_resource]
impl CacheStore {}

/// `read(key, fallback)` returns a promise for the cached value. On a miss it calls
/// `fallback(key)` once, however many reads wait for it, caches what its promise fulfills with,
/// and passes a rejection through to every waiting read without caching it.
#[jsg_resource]
struct ReadThroughCache {
    store: jsg::Rc<CacheStore>,
}

/// What the reactions to a fallback's promise need.
struct Fill {
    store: jsg::Rc<CacheStore>,
    key: String,
}

impl jsg::Traced for Fill {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.store.trace(visitor);
    }
}

#[jsg_resource]
impl ReadThroughCache {
    #[jsg_method]
    fn read<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        key: String,
        fallback: v8::Local<'s, v8::Value>,
    ) -> Result<v8::Local<'s, v8::Promise>, jsg::Error> {
        let cached = self
            .store
            .entries
            .borrow()
            .get(&key)
            .map(|value| value.as_local(lock));
        if let Some(value) = cached {
            return v8::Promise::resolved(lock, value);
        }
        let in_flight = self
            .store
            .in_flight
            .borrow()
            .get(&key)
            .map(|promise| promise.as_local(lock));
        if let Some(promise) = in_flight {
            return Ok(promise);
        }

        let fallback = v8::Local::<v8::Function>::try_from(fallback)
            .map_err(|_| jsg::Error::new_type_error("fallback must be a function"))?;
        let key_js = key.as_str().to_js(lock);
        // No RefCell borrow is held here: the fallback is JavaScript and may call `read` again.
        let produced = fallback.call::<v8::Local<v8::Value>>(lock, None, &[key_js])?;
        let produced = v8::Local::<v8::Promise>::from_js(lock, produced)?;
        // A rejection passes through `filled` unchanged. The `catch` reaction only forgets the
        // fallback; the promise it returns is dropped, and fulfilled, so nothing reports it.
        let filled = produced.then(
            lock,
            Fill {
                store: self.store.clone(),
                key: key.clone(),
            },
            |lock, fill, value| {
                let entry = value.to_traced_global(lock);
                fill.store.in_flight.borrow_mut().remove(&fill.key);
                fill.store
                    .entries
                    .borrow_mut()
                    .insert(fill.key.clone(), entry);
                Ok(value)
            },
        )?;
        produced.catch(
            lock,
            Fill {
                store: self.store.clone(),
                key: key.clone(),
            },
            |lock, fill, _reason| {
                fill.store.in_flight.borrow_mut().remove(&fill.key);
                Ok(lock.undefined())
            },
        )?;
        let in_flight = filled.to_traced_global(lock);
        self.store.in_flight.borrow_mut().insert(key, in_flight);
        Ok(filled)
    }
}

fn install_cache(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let cache = jsg::Rc::new(ReadThroughCache {
        store: jsg::Rc::new(CacheStore {
            entries: RefCell::new(HashMap::new()),
            in_flight: RefCell::new(HashMap::new()),
        }),
    });
    ctx.set_global(lock, "cache", cache);
}

#[test]
fn cache_coalesces_reads_and_caches_fulfillment() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_cache(lock, ctx);
        let code = format!(
            "{RECORD}
            globalThis.calls = 0;
            globalThis.fallback = (key) => {{
                calls++;
                return new Promise((resolve) => {{ globalThis.fill = () => resolve(key.toUpperCase()); }});
            }};
            const a = cache.read('a', fallback);
            const b = cache.read('a', fallback);
            log.push(`same promise: ${{a === b}}`, `calls: ${{calls}}`);
            record(a);
            record(b);
            fill();"
        );
        assert_eq!(
            log_after_microtasks(lock, ctx, &code),
            "same promise: true|calls: 1|fulfilled: A|fulfilled: A"
        );
        // The store has no wrapper of its own: the cache traces it, and with it the cached value.
        crate::Harness::request_gc(lock);
        // Served from the cache: the fallback does not run again.
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(cache.read('a', fallback)); log.push(`calls: ${{calls}}`)")
            ),
            "calls: 1|fulfilled: A"
        );
        Ok(())
    });
}

#[test]
fn cache_accepts_plain_values_from_fallback() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_cache(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(cache.read('k', (key) => `value of ${{key}}`))")
            ),
            "fulfilled: value of k"
        );
        Ok(())
    });
}

#[test]
fn cache_passes_rejection_through_without_caching_it() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_cache(lock, ctx);
        let code = format!(
            "{RECORD}
            globalThis.error = new TypeError('backend down');
            globalThis.calls = 0;
            cache.read('k', () => {{ calls++; return Promise.reject(error); }})
                .catch((e) => log.push(`same error: ${{e === error}}`))
                .then(() => record(cache.read('k', () => {{ calls++; return 'recovered'; }})))
                .then(() => log.push(`calls: ${{calls}}`));"
        );
        assert_eq!(
            log_after_microtasks(lock, ctx, &code),
            "same error: true|fulfilled: recovered|calls: 2"
        );
        Ok(())
    });
}

#[test]
fn cache_rejects_non_function_fallback() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_cache(lock, ctx);
        let code =
            "try { cache.read('k', 42); 'no error' } catch (e) { `${e.name}: ${e.message}` }";
        assert_eq!(
            ctx.eval::<String>(lock, code).unwrap(),
            "TypeError: fallback must be a function"
        );
        Ok(())
    });
}
