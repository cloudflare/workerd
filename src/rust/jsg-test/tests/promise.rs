// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JavaScript promises in Rust: creating and settling them through `v8::PromiseResolver`, and
//! reacting to them with `Local<Promise>::then`.

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
        assert_eq!(jsg::Number::from_js(lock, result)?.value(), 42.0);
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
        assert!(promise.result(lock).unwrap() == reason.into());
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
        assert!(v8::Local::<v8::Value>::from(same) == existing);

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

#[test]
fn then_runs_callbacks_as_microtasks_with_their_data() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = ctx.eval_raw(lock, "Promise.resolve('world')").unwrap();
        let promise = promise.try_as::<v8::Promise>().unwrap();
        let greeted = promise
            .then(
                lock,
                "hello".to_owned(),
                |lock, greeting, value| {
                    let name = String::from_js(lock, value)?;
                    Ok(format!("{greeting}, {name}").to_js(lock))
                },
                |_lock, _, reason| Ok(reason),
            )
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
        let fulfilled = fulfilled.try_as::<v8::Promise>().unwrap();
        let failed = fulfilled
            .then(
                lock,
                (),
                |_lock, (), _value| Err(jsg::Error::new_range_error("out of range")),
                |_lock, (), reason| Ok(reason),
            )
            .unwrap();
        ctx.set_global(lock, "failed", failed);

        let rejected = ctx
            .eval_raw(lock, "Promise.reject(new TypeError('nope'))")
            .unwrap();
        let rejected = rejected.try_as::<v8::Promise>().unwrap();
        let recovered = rejected
            .then(
                lock,
                (),
                |_lock, (), value| Ok(value),
                |lock, (), reason| {
                    let err = jsg::Error::from_js(lock, reason)?;
                    Ok(format!("recovered from {}", err.message).to_js(lock))
                },
            )
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

#[test]
fn panic_in_then_callback_is_an_internal_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let promise = ctx.eval_raw(lock, "Promise.resolve(1)").unwrap();
        let promise = promise.try_as::<v8::Promise>().unwrap();
        let panicked = promise
            .then(
                lock,
                (),
                |_lock, (), _value| -> Result<v8::Local<v8::Value>, jsg::Error> {
                    panic!("intentional panic in a then callback");
                },
                |_lock, (), reason| Ok(reason),
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
            .then(
                lock,
                DropCounter(&SETTLED_DATA_DROPS),
                |lock, _, _| Ok(lock.undefined()),
                |lock, _, _| Ok(lock.undefined()),
            )
            .unwrap();
        // The pending promise keeps its reactions, and with them the data.
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

/// The cache's state, shared by the resource and the reactions to in-flight fallbacks.
#[derive(Default)]
struct CacheState {
    entries: RefCell<HashMap<String, v8::TracedGlobal<v8::Value>>>,
    in_flight: RefCell<HashMap<String, v8::TracedGlobal<v8::Promise>>>,
}

impl jsg::Traced for CacheState {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.entries.trace(visitor);
        self.in_flight.trace(visitor);
    }
}

/// `read(key, fallback)` returns a promise for the cached value. On a miss it calls
/// `fallback(key)` once, however many reads wait for it, caches what its promise fulfills with,
/// and passes a rejection through to every waiting read without caching it.
#[jsg_resource]
struct ReadThroughCache {
    state: std::rc::Rc<CacheState>,
}

/// What the reactions to a fallback's promise need.
struct Fill {
    state: std::rc::Rc<CacheState>,
    key: String,
}

impl jsg::Traced for Fill {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.state.trace(visitor);
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
            .state
            .entries
            .borrow()
            .get(&key)
            .map(|value| value.as_local(lock));
        if let Some(value) = cached {
            return v8::Promise::resolved(lock, value);
        }
        let in_flight = self
            .state
            .in_flight
            .borrow()
            .get(&key)
            .map(|promise| promise.as_local(lock));
        if let Some(promise) = in_flight {
            return Ok(promise);
        }

        let fallback = fallback
            .try_as::<v8::Function>()
            .ok_or_else(|| jsg::Error::new_type_error("fallback must be a function"))?;
        let key_js = key.as_str().to_js(lock);
        // No RefCell borrow is held here: the fallback is JavaScript and may call `read` again.
        let produced = fallback.call::<v8::Local<v8::Value>>(lock, None, &[key_js])?;
        let produced = v8::Local::<v8::Promise>::from_js(lock, produced)?;
        let filled = produced.then(
            lock,
            Fill {
                state: std::rc::Rc::clone(&self.state),
                key: key.clone(),
            },
            |lock, fill, value| {
                fill.state.in_flight.borrow_mut().remove(&fill.key);
                fill.state
                    .entries
                    .borrow_mut()
                    .insert(fill.key.clone(), value.to_traced_global(lock));
                Ok(value)
            },
            |lock, fill, reason| {
                fill.state.in_flight.borrow_mut().remove(&fill.key);
                // Resolving with a rejected promise passes `reason` on unchanged; an `Err`
                // would reject with a new exception instead.
                let rejected = v8::Promise::rejected(lock, reason)?;
                Ok(rejected.into())
            },
        )?;
        self.state
            .in_flight
            .borrow_mut()
            .insert(key, filled.to_traced_global(lock));
        Ok(filled)
    }
}

fn install_cache(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let cache = jsg::Rc::new(ReadThroughCache {
        state: std::rc::Rc::default(),
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
