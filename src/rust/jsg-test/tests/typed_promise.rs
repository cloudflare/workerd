// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Typed promises, `jsg::Promise<'s, T>`: carrying Rust values, reacting to them, and converting
//! them where they cross into and out of JavaScript as C++ JSG's `jsg::Promise<T>` does.

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

/// A Rust value with no JavaScript form, which only a typed promise can carry.
struct Token {
    id: u64,
    name: String,
}

impl jsg::Traced for Token {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

// =============================================================================
// Carrying Rust values
// =============================================================================

#[test]
fn resolved_promise_carries_a_rust_value_to_its_reaction() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let token = Token {
            id: 7,
            name: "seven".to_owned(),
        };
        let promise = jsg::Promise::<Token>::resolved(lock, token)?;
        let described: jsg::Promise<String> = promise.then(lock, (), |_lock, (), token| {
            Ok(format!("{}: {}", token.id, token.name))
        })?;
        let Err(described) = described.try_consume_resolved(lock) else {
            panic!("reactions never run synchronously");
        };
        crate::Harness::run_microtasks(lock);
        let Ok(value) = described.try_consume_resolved(lock) else {
            panic!("the reaction did not run");
        };
        assert_eq!(value, "7: seven");
        Ok(())
    });
}

#[test]
fn resolver_settles_a_typed_promise() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let (promise, resolver) = jsg::Promise::<Token>::new_with_resolver(lock)?;
        let ids: jsg::Promise<u64> =
            promise.then(lock, 100_u64, |_lock, offset, token| Ok(token.id + offset))?;
        resolver.resolve(
            lock,
            Token {
                id: 1,
                name: String::new(),
            },
        )?;
        crate::Harness::run_microtasks(lock);
        let Ok(id) = ids.try_consume_resolved(lock) else {
            panic!("the reaction did not run");
        };
        assert_eq!(id, 101);
        Ok(())
    });
}

#[test]
fn then_chains_change_the_carried_type() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = jsg::Promise::<u32>::resolved(lock, 20)?;
        let doubled: jsg::Promise<u64> =
            promise.then(lock, (), |_lock, (), n| Ok(u64::from(n) * 2))?;
        let named: jsg::Promise<Token> =
            doubled.then(lock, "n".to_owned(), |_lock, prefix, n| {
                Ok(Token {
                    id: n,
                    name: format!("{prefix}{n}"),
                })
            })?;
        let shown: jsg::Promise<String> =
            named.then(lock, (), |_lock, (), token| Ok(token.name))?;
        ctx.set_global(lock, "shown", shown);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(shown)")),
            "fulfilled: n40"
        );
        Ok(())
    });
}

#[test]
fn callback_errors_reject_and_catch_recovers() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = jsg::Promise::<u32>::resolved(lock, 1)?;
        let failed: jsg::Promise<u32> = promise.then(lock, (), |_lock, (), _n| {
            Err(jsg::Error::new_range_error("out of range"))
        })?;
        let recovered = failed.catch(lock, (), |lock, (), reason| {
            let err = jsg::Error::from_js(lock, reason)?;
            Ok(err.message.len() as u32)
        })?;
        ctx.set_global(lock, "recovered", recovered);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(recovered)")),
            "fulfilled: 12"
        );
        Ok(())
    });
}

#[test]
fn then_or_catch_runs_the_callback_for_the_outcome() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let fulfilled = jsg::Promise::<u32>::resolved(lock, 3)?;
        let rejected =
            jsg::Promise::<u32>::rejected_with_error(lock, &jsg::Error::new_type_error("nope"))?;
        for (name, promise) in [("fulfilled", fulfilled), ("rejected", rejected)] {
            let outcome: jsg::Promise<String> = promise.then_or_catch(
                lock,
                (),
                |_lock, (), n| Ok(format!("value {n}")),
                |lock, (), reason| {
                    let err = jsg::Error::from_js(lock, reason)?;
                    Ok(format!("error {}", err.message))
                },
            )?;
            ctx.set_global(lock, name, outcome);
        }
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(fulfilled).then(() => record(rejected))")
            ),
            "fulfilled: value 3|fulfilled: error nope"
        );
        Ok(())
    });
}

#[test]
fn resolver_follows_a_typed_promise() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let (inner, inner_resolver) = jsg::Promise::<String>::new_with_resolver(lock)?;
        let (outer, outer_resolver) = jsg::Promise::<String>::new_with_resolver(lock)?;
        outer_resolver.resolve_with_promise(lock, inner)?;
        inner_resolver.resolve(lock, "followed".to_owned())?;
        ctx.set_global(lock, "outer", outer);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(outer)")),
            "fulfilled: followed"
        );
        Ok(())
    });
}

static CARRIED_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Counts its drops in a static counter.
struct Carried;

impl Drop for Carried {
    fn drop(&mut self) {
        CARRIED_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

impl jsg::Traced for Carried {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

/// The reaction takes the value out of its wrapper, so the value is dropped when the callback is
/// done with it, and the wrapper, which the garbage collector frees later, holds nothing.
#[test]
fn carried_value_is_moved_into_the_reaction() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let promise = jsg::Promise::<Carried>::resolved(lock, Carried)?;
        let _: jsg::Promise<()> = promise.then(lock, (), |_lock, (), carried| {
            assert_eq!(CARRIED_DROPS.load(Ordering::SeqCst), 0);
            drop(carried);
            Ok(())
        })?;
        crate::Harness::run_microtasks(lock);
        assert_eq!(CARRIED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(CARRIED_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

static UNTAKEN_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Counts its drops in a static counter.
struct Untaken;

impl Drop for Untaken {
    fn drop(&mut self) {
        UNTAKEN_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

impl jsg::Traced for Untaken {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

/// A value that no reaction takes is dropped with its wrapper, by the garbage collector.
#[test]
fn untaken_value_is_dropped_with_its_promise() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let _promise = jsg::Promise::<Untaken>::resolved(lock, Untaken)?;
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert_eq!(UNTAKEN_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// Holds a JavaScript object that only it references.
#[jsg_resource]
struct Holder {
    object: v8::TracedGlobal<v8::Object>,
}

#[jsg_resource]
impl Holder {}

/// Keeps what a reaction hands it, as a resource field.
#[jsg_resource]
struct Keeper {
    kept: jsg::RefCell<Option<jsg::Rc<Holder>>>,
}

#[jsg_resource]
impl Keeper {
    #[jsg_method]
    fn state(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        let object = self
            .kept
            .borrow()
            .as_ref()
            .map(|holder| holder.object.as_local(lock));
        let object = object.ok_or_else(|| jsg::Error::new_type_error("nothing kept"))?;
        let state = object.get(lock, "state")?;
        String::from_js(lock, state)
    }
}

/// A collection while a typed promise is pending leaves the `jsg::Rc` it carries weak, for the
/// wrapper to trace. The reaction gets it strong again, with no parent, as moving a C++
/// `jsg::Ref` does, so it can go in another resource and survive collections there.
#[test]
fn carried_resource_can_be_kept_after_garbage_collection() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let keeper = jsg::Rc::new(
            lock,
            Keeper {
                kept: jsg::RefCell::new(None),
            },
        );
        ctx.set_global(lock, "keeper", keeper.clone());
        let object = lock.new_object();
        let alive = "alive".to_js(lock)?;
        object.set(lock, "state", alive)?;
        let object = object.to_traced_global(lock);
        let holder = jsg::Rc::new(lock, Holder { object });
        let (promise, resolver) = jsg::Promise::<jsg::Rc<Holder>>::new_with_resolver(lock)?;
        let kept: jsg::Promise<()> = promise.then(lock, keeper, |lock, keeper, holder| {
            *keeper.kept.borrow_mut(lock) = Some(holder);
            // Traces the keeper, and through it the `jsg::Rc`, from its new owner.
            crate::Harness::request_gc(lock);
            Ok(())
        })?;
        resolver.resolve(lock, holder)?;
        // Only the opaque wrapper holds the resource now, and the collection makes it weak.
        crate::Harness::request_gc(lock);
        ctx.set_global(lock, "kept", kept);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(kept.then(() => keeper.state()))")
            ),
            "fulfilled: alive"
        );
        crate::Harness::request_gc(lock);
        assert_eq!(ctx.eval::<String>(lock, "keeper.state()").unwrap(), "alive");
        Ok(())
    });
}

/// The same for a `TracedGlobal` carried as it is: the reaction gets its handle strong again, so
/// its value survives a collection while the reaction holds it.
#[test]
fn carried_traced_global_survives_garbage_collection_after_it_is_taken() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let value = lock.undefined();
        let outer = v8::Promise::resolved(lock, value)?;
        // The object is created in a reaction, whose handles go once it returns, so that only the
        // typed promise refers to it after that.
        let read = outer.then(lock, (), |lock, (), _| {
            let object = lock.new_object();
            let alive = "alive".to_js(lock)?;
            object.set(lock, "state", alive)?;
            let global = object.to_traced_global(lock);
            let carried = jsg::Promise::<v8::TracedGlobal<v8::Object>>::resolved(lock, global)?;
            let state: jsg::Promise<String> = carried.then(lock, (), |lock, (), global| {
                crate::Harness::request_gc(lock);
                let object = global.as_local(lock);
                let state = object.get(lock, "state")?;
                String::from_js(lock, state)
            })?;
            // Makes the handle weak, for the wrapper to trace.
            crate::Harness::request_gc(lock);
            state.to_js(lock)
        })?;
        ctx.set_global(lock, "read", read);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(read)")),
            "fulfilled: alive"
        );
        Ok(())
    });
}

/// JavaScript that replaced `Promise.prototype.then` can settle a typed promise that was resolved
/// with another, since V8 calls that `then` to follow the other. A value that is not the
/// promise's own then rejects the reaction with an internal error, as in C++ JSG.
#[test]
fn value_settled_from_javascript_rejects_with_an_internal_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(
            lock,
            "globalThis.originalThen = Promise.prototype.then;
             Promise.prototype.then = function(resolve) {
               Promise.prototype.then = originalThen;
               resolve(42);
             };",
        )
        .unwrap();
        let (inner, _inner_resolver) = jsg::Promise::<Token>::new_with_resolver(lock)?;
        let (outer, outer_resolver) = jsg::Promise::<Token>::new_with_resolver(lock)?;
        outer_resolver.resolve_with_promise(lock, inner)?;
        let name: jsg::Promise<String> = outer.then(lock, (), |_lock, (), token| Ok(token.name))?;
        ctx.set_global(lock, "name", name);
        // V8 calls the replaced `then` from a microtask.
        crate::Harness::run_microtasks(lock);
        let restored = ctx
            .eval::<bool>(lock, "Promise.prototype.then === originalThen")
            .unwrap();
        assert!(restored, "V8 did not call the replaced then");
        let log = log_after_microtasks(lock, ctx, &format!("{RECORD} record(name)"));
        assert!(log.starts_with("rejected: Error: internal error"), "{log}");
        Ok(())
    });
}

#[test]
fn catch_passes_the_value_through() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = jsg::Promise::<Token>::resolved(
            lock,
            Token {
                id: 3,
                name: "through".to_owned(),
            },
        )?;
        let caught = promise.catch(lock, (), |_lock, (), _reason| {
            Err(jsg::Error::new_type_error("on_rejected ran"))
        })?;
        let name: jsg::Promise<String> = caught.then(lock, (), |_lock, (), token| {
            Ok(format!("{} {}", token.id, token.name))
        })?;
        ctx.set_global(lock, "name", name);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(name)")),
            "fulfilled: 3 through"
        );
        Ok(())
    });
}

#[test]
fn try_consume_resolved_returns_pending_and_rejected_promises() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let (pending, _resolver) = jsg::Promise::<u32>::new_with_resolver(lock)?;
        assert!(pending.try_consume_resolved(lock).is_err());
        let mut rejected =
            jsg::Promise::<u32>::rejected_with_error(lock, &jsg::Error::new_type_error("no"))?;
        rejected.mark_as_handled();
        assert!(rejected.try_consume_resolved(lock).is_err());
        Ok(())
    });
}

/// A value whose conversion to JavaScript fails.
struct Unconvertible;

impl jsg::Traced for Unconvertible {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

impl<'s> ToJS<'s> for Unconvertible {
    fn to_js(self, _lock: &mut jsg::Lock<'s>) -> Result<v8::Local<'s, v8::Value>, jsg::Error> {
        Err(jsg::Error::new_range_error("cannot convert"))
    }
}

/// The conversion runs in a reaction, so its failure rejects the promise JavaScript gets.
#[test]
fn to_js_rejects_with_a_failed_conversion() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = jsg::Promise::<Unconvertible>::resolved(lock, Unconvertible)?;
        ctx.set_global(lock, "p", promise);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(p)")),
            "rejected: RangeError: cannot convert"
        );
        Ok(())
    });
}

#[test]
fn when_resolved_keeps_the_promise() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = jsg::Promise::<u32>::resolved(lock, 5)?;
        let settled = promise.when_resolved();
        let value: jsg::Promise<u32> = promise.then(lock, (), |_lock, (), n| Ok(n + 1))?;
        ctx.set_global(lock, "settled", settled);
        ctx.set_global(lock, "value", value);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(settled).then(() => record(value))")
            ),
            "fulfilled: undefined|fulfilled: 6"
        );
        Ok(())
    });
}

// =============================================================================
// ToJS: what JavaScript gets
// =============================================================================

/// As C++ `PromiseWrapper::wrap()` does, JavaScript gets the promise of a reaction that converts
/// the value, which settles one microtask after the typed promise: a JavaScript reaction to a
/// promise that was already settled runs after one to an untyped promise registered later.
#[test]
fn to_js_converts_in_a_reaction() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let typed = jsg::Promise::<String>::resolved(lock, "typed".to_owned())?;
        ctx.set_global(lock, "typed", typed);
        let typed = ctx.eval_raw(lock, "typed").unwrap();
        let typed = v8::Local::<v8::Promise>::try_from(typed).unwrap();
        // The reaction has not run yet.
        assert_eq!(typed.state(), PromiseState::Pending);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                "typed.then((v) => log.push(v));
                 Promise.resolve('untyped').then((v) => log.push(v));"
            ),
            "untyped|typed"
        );
        Ok(())
    });
}

#[test]
fn to_js_never_exposes_the_carried_wrapper() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let typed = jsg::Promise::<u32>::resolved(lock, 9)?;
        ctx.set_global(lock, "typed", typed);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                "typed.then((v) => log.push(`${typeof v} ${v}`));"
            ),
            "number 9"
        );
        Ok(())
    });
}

#[test]
fn to_js_passes_a_rejection_through() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let rejected = jsg::Promise::<u32>::rejected_with_error(
            lock,
            &jsg::Error::new_type_error("rejected"),
        )?;
        ctx.set_global(lock, "rejected", rejected);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(rejected)")),
            "rejected: TypeError: rejected"
        );
        Ok(())
    });
}

/// Marking a typed promise as handled marks the promise JavaScript gets too, as in C++.
#[test]
fn to_js_propagates_handled() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let mut handled =
            jsg::Promise::<u32>::rejected_with_error(lock, &jsg::Error::new_type_error("handled"))?;
        handled.mark_as_handled();
        let handled = handled.to_js(lock)?;
        let handled = v8::Local::<v8::Promise>::try_from(handled).unwrap();
        assert!(handled.has_handler());

        let unhandled = jsg::Promise::<u32>::rejected_with_error(
            lock,
            &jsg::Error::new_type_error("unhandled"),
        )?
        .to_js(lock)?;
        let unhandled = v8::Local::<v8::Promise>::try_from(unhandled).unwrap();
        assert!(!unhandled.has_handler());
        unhandled.mark_as_handled();
        Ok(())
    });
}

// =============================================================================
// FromJS: what Rust gets
// =============================================================================

/// Converts its argument to a `jsg::Promise<u32>` and doubles it.
#[jsg_resource]
struct Doubler;

#[jsg_resource]
impl Doubler {
    #[jsg_method]
    fn double<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: jsg::Promise<'s, u32>,
    ) -> Result<jsg::Promise<'s, u32>, jsg::Error> {
        promise.then(lock, (), |_lock, (), n| Ok(n * 2))
    }

    #[jsg_method]
    fn leaf<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: jsg::Promise<'s, jsg::Rc<Leaf>>,
    ) -> Result<jsg::Promise<'s, String>, jsg::Error> {
        promise.then(lock, (), |_lock, (), leaf| Ok(leaf.name.clone()))
    }

    #[jsg_method]
    fn ignore<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: jsg::Promise<'s, ()>,
    ) -> Result<jsg::Promise<'s, ()>, jsg::Error> {
        promise.then(lock, (), |_lock, (), ()| Ok(()))
    }

    #[jsg_method]
    fn keep<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        promise: jsg::Promise<'s, v8::Value>,
    ) -> Result<jsg::Promise<'s, v8::Value>, jsg::Error> {
        promise.then(lock, (), |_lock, (), value| Ok(value))
    }
}

/// A resource for a typed promise to carry.
#[jsg_resource]
struct Leaf {
    name: String,
}

#[jsg_resource]
impl Leaf {}

fn install_doubler(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let doubler = jsg::Rc::new(lock, Doubler);
    ctx.set_global(lock, "doubler", doubler);
    let leaf = jsg::Rc::new(
        lock,
        Leaf {
            name: "leaf".to_owned(),
        },
    );
    ctx.set_global(lock, "leaf", leaf);
}

#[test]
fn from_js_converts_the_resolution() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(doubler.double(Promise.resolve(21)))")
            ),
            "fulfilled: 42"
        );
        Ok(())
    });
}

/// A promise that resolves to a value of the wrong type rejects with C++ JSG's error.
#[test]
fn from_js_rejects_a_resolution_of_the_wrong_type() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(doubler.leaf(Promise.resolve(leaf)))
                         .then(() => record(doubler.leaf(Promise.resolve({{}}))))"
                )
            ),
            "fulfilled: leaf|rejected: TypeError: Incorrect type for Promise: the Promise did not \
             resolve to 'Leaf'."
        );
        Ok(())
    });
}

/// A conversion that fails rather than finding the wrong type rejects with its own error, as in
/// C++ JSG, whose conversions throw too.
#[test]
fn from_js_rejects_with_a_failed_conversion() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(doubler.double(Promise.resolve({{}})))")
            ),
            "rejected: TypeError: The value cannot be converted because it is not an integer."
        );
        Ok(())
    });
}

#[test]
fn from_js_passes_a_rejection_through() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} globalThis.error = new RangeError('upstream');
                     doubler.double(Promise.reject(error))
                         .catch((e) => log.push(`same error: ${{e === error}}`))"
                )
            ),
            "same error: true"
        );
        Ok(())
    });
}

/// A value that is not a promise is converted at once, so that one of the wrong type throws.
#[test]
fn from_js_converts_a_plain_value_at_once() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(doubler.double(4))")),
            "fulfilled: 8"
        );
        let thrown = ctx
            .eval::<String>(
                lock,
                "try { doubler.double({}); 'no error' } catch (e) { `${e.name}` }",
            )
            .unwrap();
        assert_eq!(thrown, "TypeError");
        Ok(())
    });
}

/// Without `unwrap_custom_thenables`, a thenable is a plain value, as in C++ JSG.
#[test]
fn from_js_does_not_follow_thenables_by_default() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} globalThis.thenCalls = 0;
                     const thenable = {{ then(resolve) {{ thenCalls++; resolve(5); }} }};
                     record(doubler.keep(thenable).then((v) => v === thenable));
                     Promise.resolve().then(() => log.push(`calls: ${{thenCalls}}`));"
                )
            ),
            // `keep` resolves with the thenable, which the native promise then follows.
            "calls: 1|fulfilled: false"
        );
        Ok(())
    });
}

#[test]
fn from_js_follows_thenables_with_unwrap_custom_thenables() {
    let harness = crate::Harness::with_flags(|mut flags| flags.set_unwrap_custom_thenables(true));
    harness.run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(doubler.double({{ then(resolve) {{ resolve(5); }} }}))")
            ),
            "fulfilled: 10"
        );
        Ok(())
    });
}

/// As in C++ JSG, a `has` trap that throws makes the conversion throw, and a `then` getter that
/// throws rejects the promise.
#[test]
fn from_js_passes_on_what_a_thenable_throws() {
    let harness = crate::Harness::with_flags(|mut flags| flags.set_unwrap_custom_thenables(true));
    harness.run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            ctx.eval::<String>(
                lock,
                "try {
                   doubler.double(new Proxy({}, { has() { throw new Error('trap'); } }));
                   'no error'
                 } catch (e) { e.message }"
            )
            .unwrap(),
            "trap"
        );
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(doubler.double({{ get then() {{ throw new Error('getter'); }} }}))"
                )
            ),
            "rejected: Error: getter"
        );
        Ok(())
    });
}

/// As for `jsg::Promise<void>`, any value is accepted, and the promise resolves to `undefined`.
#[test]
fn void_promise_accepts_any_value() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(doubler.ignore(Promise.resolve('dropped')))
                         .then(() => record(doubler.ignore(42)))"
                )
            ),
            "fulfilled: undefined|fulfilled: undefined"
        );
        Ok(())
    });
}

/// As for `jsg::Promise<jsg::Value>`, the value is carried as it is.
#[test]
fn value_promise_carries_javascript_values() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install_doubler(lock, ctx);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                "globalThis.object = {};
                 doubler.keep(Promise.resolve(object)).then((v) => log.push(v === object));
                 doubler.keep('plain').then((v) => log.push(v));"
            ),
            "true|plain"
        );
        Ok(())
    });
}

#[test]
fn from_js_names_promise_and_rejects_other_values() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        assert_eq!(<jsg::Promise<u32> as FromJS>::type_name(), "Promise");
        let value = "not a leaf".to_js(lock)?;
        let Err(err) = jsg::Promise::<jsg::Rc<Leaf>>::from_js(lock, value) else {
            panic!("a string converted to a Promise<Leaf>");
        };
        assert_eq!(err.message, "Expected Promise but got string");
        Ok(())
    });
}

/// The garbage collector traces what a typed promise carries, through its wrapper.
#[test]
fn carried_resource_survives_garbage_collection() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let leaf = jsg::Rc::new(
            lock,
            Leaf {
                name: "kept".to_owned(),
            },
        );
        let promise = jsg::Promise::<jsg::Rc<Leaf>>::resolved(lock, leaf)?;
        let name: jsg::Promise<String> =
            promise.then(lock, (), |_lock, (), leaf| Ok(leaf.name.clone()))?;
        ctx.set_global(lock, "name", name);
        crate::Harness::request_gc(lock);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(name)")),
            "fulfilled: kept"
        );
        Ok(())
    });
}

// =============================================================================
// Persistent promises and eval_now
// =============================================================================

/// Hands out a typed promise and settles it from a later call, holding the resolver in between.
#[jsg_resource]
struct Deferred {
    resolver: jsg::RefCell<Option<jsg::TracedPromiseResolver<Token>>>,
    promise: jsg::RefCell<Option<jsg::TracedPromise<Token>>>,
}

#[jsg_resource]
impl Deferred {
    #[jsg_method]
    fn start(&self, lock: &mut jsg::Lock<'_>) -> Result<(), jsg::Error> {
        let (promise, resolver) = jsg::Promise::<Token>::new_with_resolver(lock)?;
        *self.resolver.borrow_mut(lock) = Some(resolver.into_traced(lock));
        *self.promise.borrow_mut(lock) = Some(promise.into_traced(lock));
        Ok(())
    }

    #[jsg_method]
    fn name<'s>(&self, lock: &mut jsg::Lock<'s>) -> Result<jsg::Promise<'s, String>, jsg::Error> {
        let promise = self.promise.borrow_mut(lock).take();
        let promise = promise
            .ok_or_else(|| jsg::Error::new_type_error("not started"))?
            .into_local(lock);
        promise.then(lock, (), |_lock, (), token| Ok(token.name))
    }

    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>, name: String) -> Result<(), jsg::Error> {
        let resolver = self.resolver.borrow_mut(lock).take();
        let resolver = resolver
            .ok_or_else(|| jsg::Error::new_type_error("not started"))?
            .as_local(lock);
        resolver.resolve(lock, Token { id: 0, name })
    }
}

#[test]
fn resource_keeps_typed_promise_and_resolver() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let deferred = jsg::Rc::new(
            lock,
            Deferred {
                resolver: jsg::RefCell::new(None),
                promise: jsg::RefCell::new(None),
            },
        );
        ctx.set_global(lock, "deferred", deferred);
        ctx.eval_raw(lock, "deferred.start()").unwrap();
        // The resource's fields alone keep the promise and its resolver.
        crate::Harness::request_gc(lock);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(deferred.name()); deferred.finish('done');")
            ),
            "fulfilled: done"
        );
        Ok(())
    });
}

/// Returns a promise, failing as `eval_now` turns into a rejection.
#[jsg_resource]
struct Evaluator;

#[jsg_resource]
impl Evaluator {
    /// Requests a termination, which V8 delivers while `spin` runs, then calls `eval_now`.
    #[jsg_method]
    fn terminating<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        spin: v8::Local<'s, v8::Value>,
    ) -> Result<jsg::Promise<'s, u32>, jsg::Error> {
        lock.terminate_execution();
        let spin = v8::Local::<v8::Function>::try_from(spin).unwrap();
        let err = spin.call(lock, None, &[]).unwrap_err();
        assert!(err.is_pending());
        let result = lock.eval_now(|lock| jsg::Promise::<u32>::resolved(lock, 1));
        let Err(err) = result else {
            panic!("eval_now created a promise while terminating");
        };
        assert!(err.is_pending());
        Err(err)
    }

    #[jsg_method]
    fn evaluate<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        how: String,
        callback: v8::Local<'s, v8::Value>,
    ) -> Result<jsg::Promise<'s, u32>, jsg::Error> {
        lock.eval_now(|lock| match how.as_str() {
            "ok" => jsg::Promise::resolved(lock, 1),
            "error" => Err(jsg::Error::new_range_error("failed")),
            _ => {
                let callback = v8::Local::<v8::Function>::try_from(callback).unwrap();
                callback.call(lock, None, &[])?;
                jsg::Promise::resolved(lock, 2)
            }
        })
    }
}

/// While execution is terminating, `eval_now` creates no promise and passes on the termination.
#[test]
fn eval_now_keeps_the_termination() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let evaluator = jsg::Rc::new(lock, Evaluator);
        ctx.set_global(lock, "evaluator", evaluator);
        let result = ctx.eval_raw(
            lock,
            "globalThis.after = false;
             try { evaluator.terminating(() => { while (true) {} }); } catch (e) {}
             globalThis.after = true;",
        );
        assert!(result.is_err(), "the script was not terminated");
        assert!(!ctx.eval::<bool>(lock, "globalThis.after").unwrap());
        Ok(())
    });
}

#[test]
fn eval_now_rejects_with_what_fails() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let evaluator = jsg::Rc::new(lock, Evaluator);
        ctx.set_global(lock, "evaluator", evaluator);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} globalThis.error = new TypeError('thrown');
                     record(evaluator.evaluate('ok', null))
                         .then(() => record(evaluator.evaluate('error', null)))
                         .then(() => evaluator.evaluate('call', () => {{ throw error; }}))
                         .catch((e) => log.push(`same error: ${{e === error}}`));"
                )
            ),
            "fulfilled: 1|rejected: RangeError: failed|same error: true"
        );
        Ok(())
    });
}

// =============================================================================
// Callbacks that return promises
// =============================================================================

/// Settles a typed promise from a later call, for a callback that returns its promise.
#[jsg_resource]
struct Later {
    resolver: jsg::RefCell<Option<jsg::TracedPromiseResolver<Token>>>,
}

#[jsg_resource]
impl Later {
    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>, name: String) -> Result<(), jsg::Error> {
        let resolver = self.resolver.borrow_mut(lock).take();
        let resolver = resolver
            .ok_or_else(|| jsg::Error::new_type_error("nothing is waiting"))?
            .as_local(lock);
        resolver.resolve(lock, Token { id: 1, name })
    }

    #[jsg_method]
    fn fail(&self, lock: &mut jsg::Lock<'_>, message: String) -> Result<(), jsg::Error> {
        let resolver = self.resolver.borrow_mut(lock).take();
        let resolver = resolver
            .ok_or_else(|| jsg::Error::new_type_error("nothing is waiting"))?
            .as_local(lock);
        resolver.reject_with_error(lock, &jsg::Error::new_range_error(message))
    }
}

impl Later {
    /// A pending promise, which `finish` or `fail` settles.
    fn wait<'s>(&self, lock: &mut jsg::Lock<'s>) -> Result<jsg::Promise<'s, Token>, jsg::Error> {
        let (promise, resolver) = jsg::Promise::<Token>::new_with_resolver(lock)?;
        *self.resolver.borrow_mut(lock) = Some(resolver.into_traced(lock));
        Ok(promise)
    }
}

fn install_later(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) -> jsg::Rc<Later> {
    let later = jsg::Rc::new(
        lock,
        Later {
            resolver: jsg::RefCell::new(None),
        },
    );
    ctx.set_global(lock, "later", later.clone());
    later
}

/// As C++ JSG's `then()` chains a continuation that returns a `jsg::Promise<U>`, the promise
/// `then_promise` returns follows the callback's, which carries a Rust value through.
#[test]
fn then_promise_follows_the_callback_promise() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let later = install_later(lock, ctx);
        let start = jsg::Promise::<u32>::resolved(lock, 1)?;
        let waited: jsg::Promise<Token> =
            start.then_promise(lock, later, |lock, later, _n| later.wait(lock))?;
        let name: jsg::Promise<String> =
            waited.then(lock, (), |_lock, (), token| Ok(token.name))?;
        ctx.set_global(lock, "name", name);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(name);
                     Promise.resolve().then(() => later.finish('chained'));"
                )
            ),
            "fulfilled: chained"
        );
        Ok(())
    });
}

#[test]
fn then_promise_passes_on_the_callback_promise_rejection() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let later = install_later(lock, ctx);
        let start = jsg::Promise::<u32>::resolved(lock, 1)?;
        let waited: jsg::Promise<Token> =
            start.then_promise(lock, later, |lock, later, _n| later.wait(lock))?;
        let waited: jsg::Promise<String> =
            waited.then(lock, (), |_lock, (), token| Ok(token.name))?;
        ctx.set_global(lock, "waited", waited);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(waited);
                     Promise.resolve().then(() => later.fail('inner'));"
                )
            ),
            "rejected: RangeError: inner"
        );
        Ok(())
    });
}

#[test]
fn catch_promise_recovers_with_a_promise() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let later = install_later(lock, ctx);
        let failed =
            jsg::Promise::<Token>::rejected_with_error(lock, &jsg::Error::new_type_error("no"))?;
        let recovered =
            failed.catch_promise(lock, later, |lock, later, _reason| later.wait(lock))?;
        let name: jsg::Promise<String> =
            recovered.then(lock, (), |_lock, (), token| Ok(token.name))?;
        ctx.set_global(lock, "name", name);
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!(
                    "{RECORD} record(name);
                     Promise.resolve().then(() => later.finish('recovered'));"
                )
            ),
            "fulfilled: recovered"
        );
        Ok(())
    });
}

#[test]
fn then_or_catch_promise_runs_the_callback_for_the_outcome() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let fulfilled = jsg::Promise::<u32>::resolved(lock, 2)?;
        let rejected =
            jsg::Promise::<u32>::rejected_with_error(lock, &jsg::Error::new_type_error("nope"))?;
        for (name, promise) in [("fulfilled", fulfilled), ("rejected", rejected)] {
            let outcome: jsg::Promise<String> = promise.then_or_catch_promise(
                lock,
                (),
                |lock, (), n| jsg::Promise::resolved(lock, format!("value {n}")),
                |lock, (), reason| {
                    let err = jsg::Error::from_js(lock, reason)?;
                    jsg::Promise::resolved(lock, format!("error {}", err.message))
                },
            )?;
            ctx.set_global(lock, name, outcome);
        }
        assert_eq!(
            log_after_microtasks(
                lock,
                ctx,
                &format!("{RECORD} record(fulfilled).then(() => record(rejected))")
            ),
            "fulfilled: value 2|fulfilled: error nope"
        );
        Ok(())
    });
}

// =============================================================================
// The creator of a returned promise
// =============================================================================

/// What settles a `Waiter`'s promise, as pending I/O would: it holds the resolver, and nothing
/// holds the `Waiter`.
#[jsg_resource]
struct Source {
    resolver: jsg::RefCell<Option<jsg::TracedPromiseResolver<u32>>>,
}

#[jsg_resource]
impl Source {
    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>, value: u32) -> Result<(), jsg::Error> {
        let resolver = self.resolver.borrow_mut(lock).take();
        let resolver = resolver
            .ok_or_else(|| jsg::Error::new_type_error("nothing is waiting"))?
            .as_local(lock);
        resolver.resolve(lock, value)
    }
}

static WAITER_DROPS: AtomicUsize = AtomicUsize::new(0);

/// Counts its drops in `WAITER_DROPS`.
struct WaiterDrops;

impl Drop for WaiterDrops {
    fn drop(&mut self) {
        WAITER_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

impl jsg::Traced for WaiterDrops {
    fn trace(&self, _visitor: &mut jsg::GcVisitor) {}
}

/// Returns promises that a `Source` settles.
#[jsg_resource]
struct Waiter {
    _drops: WaiterDrops,
}

#[jsg_resource]
impl Waiter {
    #[jsg_method]
    fn wait<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        source: jsg::Rc<Source>,
    ) -> Result<jsg::Promise<'s, u32>, jsg::Error> {
        let (promise, resolver) = jsg::Promise::<u32>::new_with_resolver(lock)?;
        *source.resolver.borrow_mut(lock) = Some(resolver.into_traced(lock));
        Ok(promise)
    }

    /// The same, in a container, which passes the creator on.
    #[jsg_method]
    fn maybe_wait<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        source: jsg::Rc<Source>,
    ) -> Result<Option<jsg::Promise<'s, u32>>, jsg::Error> {
        self.wait(lock, source).map(Some)
    }
}

/// As in C++ JSG, a promise that a method returns keeps the method's resource alive until it
/// settles, then lets it go. Each step runs in one context, whose globals would go with it, and
/// the waiter is created by a function, so that no handle of the test's scope refers to it.
fn returned_promise_keeps_its_creator_alive(method: &str) {
    WAITER_DROPS.store(0, Ordering::SeqCst);
    crate::Harness::new().run_in_context(|lock, ctx| {
        let source = jsg::Rc::new(
            lock,
            Source {
                resolver: jsg::RefCell::new(None),
            },
        );
        ctx.set_global(lock, "source", source);
        let make_waiter = v8::Function::new(lock, (), |lock, (), _args| {
            jsg::Rc::new(
                lock,
                Waiter {
                    _drops: WaiterDrops,
                },
            )
            .to_js(lock)
        })?;
        ctx.set_global(lock, "makeWaiter", make_waiter);
        ctx.eval_raw(
            lock,
            &format!("globalThis.p = makeWaiter().{method}(source); undefined"),
        )
        .unwrap();
        crate::Harness::request_gc(lock);
        assert_eq!(WAITER_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(
            log_after_microtasks(lock, ctx, &format!("{RECORD} record(p); source.finish(7);")),
            "fulfilled: 7"
        );
        crate::Harness::request_gc(lock);
        assert_eq!(WAITER_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

#[test]
fn returned_promise_keeps_its_resource_alive() {
    returned_promise_keeps_its_creator_alive("wait");
}

#[test]
fn promise_in_a_returned_container_keeps_its_resource_alive() {
    returned_promise_keeps_its_creator_alive("maybeWait");
}
