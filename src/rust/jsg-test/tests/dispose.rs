// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `#[jsg_dispose]` and `#[jsg_async_dispose]`, which install a resource method as
//! `Symbol.dispose` or `Symbol.asyncDispose` of its prototype for `using` and `await using`.

use std::cell::Cell;

use jsg::MemberRegistry;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg::v8;
use jsg_macros::jsg_async_dispose;
use jsg_macros::jsg_dispose;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

use crate::Harness;

/// Runs `code`, then the microtasks it queued, and returns what it pushed to `log`.
fn log_after_microtasks(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    ctx.eval_raw(lock, code).unwrap();
    Harness::run_microtasks(lock);
    ctx.eval(lock, "log.join('|')").unwrap()
}

#[jsg_resource]
struct Handle {
    disposals: Cell<u32>,
}

#[jsg_resource]
impl Handle {
    #[jsg_dispose]
    fn dispose(&self) {
        self.disposals.set(self.disposals.get() + 1);
    }

    #[jsg_property(readonly)]
    fn get_disposals(&self) -> u32 {
        self.disposals.get()
    }
}

fn new_handle(lock: &mut jsg::Lock) -> jsg::Rc<Handle> {
    jsg::Rc::new(
        lock,
        Handle {
            disposals: Cell::new(0),
        },
    )
}

#[test]
fn using_disposes_at_scope_exit() {
    Harness::new().run_in_context(|lock, ctx| {
        let handle = new_handle(lock);
        ctx.set_global(lock, "handle", handle);
        let result: String = ctx
            .eval(
                lock,
                "const log = [];
             { using h = handle; log.push(handle.disposals); }
             log.push(handle.disposals);
             try {
               using h = handle;
               throw new Error('boom');
             } catch (e) {
               log.push(e.message);
             }
             log.push(handle.disposals);
             handle[Symbol.dispose]();
             log.push(handle.disposals);
             log.join()",
            )
            .unwrap();
        assert_eq!(result, "0,1,boom,2,3");
        Ok(())
    });
}

#[test]
fn disposer_is_a_hidden_prototype_method() {
    Harness::new().run_in_context(|lock, ctx| {
        let handle = new_handle(lock);
        ctx.set_global(lock, "handle", handle);
        let result: String = ctx
            .eval(
                lock,
                "const proto = Object.getPrototypeOf(handle);
             const d = Object.getOwnPropertyDescriptor(proto, Symbol.dispose);
             [typeof d.value, d.enumerable, d.writable, d.configurable, d.value.length,
              JSON.stringify(d.value.name), Object.hasOwn(handle, Symbol.dispose), Symbol.asyncDispose in handle,
              Object.keys(proto).includes('dispose'), 'dispose' in handle].join()",
            )
            .unwrap();
        assert_eq!(
            result,
            "function,false,true,true,0,\"\",false,false,false,false"
        );
        Ok(())
    });
}

#[test]
fn disposer_checks_its_receiver_and_is_not_a_constructor() {
    Harness::new().run_in_context(|lock, ctx| {
        let handle = new_handle(lock);
        ctx.set_global(lock, "handle", handle);
        let failing = jsg::Rc::new(lock, Failing);
        ctx.set_global(lock, "failing", failing);
        let result: String = ctx
            .eval(
                lock,
                "const dispose = handle[Symbol.dispose];
             const errors = [];
             const calls = [() => dispose.call({}), () => dispose.call(failing), () => new dispose()];
             for (const f of calls) {
               try { f(); } catch (e) { errors.push(e instanceof TypeError); }
             }
             [...errors, handle.disposals].join()",
            )
            .unwrap();
        assert_eq!(result, "true,true,true,0");
        Ok(())
    });
}

/// `#[jsg_method]` first: `close()` and `[Symbol.dispose]()` are the same method.
#[jsg_resource]
struct CloseFirst {
    closes: Cell<u32>,
}

#[jsg_resource]
impl CloseFirst {
    #[jsg_method]
    #[jsg_dispose]
    fn close(&self) {
        self.closes.set(self.closes.get() + 1);
    }

    #[jsg_property(readonly)]
    fn get_closes(&self) -> u32 {
        self.closes.get()
    }
}

/// `#[jsg_dispose]` first, with a `this` receiver and a lock.
#[jsg_resource]
struct DisposeFirst {
    closes: Cell<u32>,
}

#[jsg_resource]
impl DisposeFirst {
    #[jsg_dispose]
    #[jsg_method]
    fn close(this: jsg::Rc<Self>, _lock: &mut jsg::Lock) {
        this.closes.set(this.closes.get() + 1);
    }

    #[jsg_property(readonly)]
    fn get_closes(&self) -> u32 {
        self.closes.get()
    }
}

#[test]
fn disposer_can_also_be_a_named_method() {
    Harness::new().run_in_context(|lock, ctx| {
        let close_first = jsg::Rc::new(
            lock,
            CloseFirst {
                closes: Cell::new(0),
            },
        );
        ctx.set_global(lock, "closeFirst", close_first);
        let dispose_first = jsg::Rc::new(
            lock,
            DisposeFirst {
                closes: Cell::new(0),
            },
        );
        ctx.set_global(lock, "disposeFirst", dispose_first);
        let result: String = ctx
            .eval(
                lock,
                "const results = [closeFirst, disposeFirst].map((c) => {
               c.close();
               { using _ = c; }
               return [typeof c.close, c.close.length, c.closes].join();
             });
             // The `this` receiver checks its type too.
             try {
               disposeFirst[Symbol.dispose].call(closeFirst);
             } catch (e) {
               results.push(e.name);
             }
             results.push(closeFirst.closes, disposeFirst.closes);
             results.join(' ')",
            )
            .unwrap();
        assert_eq!(result, "function,0,2 function,0,2 TypeError 2 2");
        Ok(())
    });
}

#[jsg_resource]
struct Failing;

#[jsg_resource]
impl Failing {
    #[jsg_dispose]
    fn dispose(&self) -> jsg::Result<()> {
        Err(jsg::Error::new_type_error("dispose failed"))
    }
}

#[test]
fn disposer_error_propagates_and_suppresses_the_scope_error() {
    Harness::new().run_in_context(|lock, ctx| {
        let failing = jsg::Rc::new(lock, Failing);
        ctx.set_global(lock, "failing", failing);
        let result: String = ctx
            .eval(
                lock,
                "const log = [];
             try { using f = failing; } catch (e) { log.push(`${e.name}: ${e.message}`); }
             try {
               using f = failing;
               throw new Error('body');
             } catch (e) {
               log.push(e.constructor.name, e.error.message, e.suppressed.message);
             }
             log.join()",
            )
            .unwrap();
        assert_eq!(
            result,
            "TypeError: dispose failed,SuppressedError,dispose failed,body"
        );
        Ok(())
    });
}

/// Disposes asynchronously, settling its promise when JavaScript calls `finish` or `fail`.
#[jsg_resource]
struct AsyncCloser {
    resolver: jsg::RefCell<Option<v8::TracedGlobal<v8::PromiseResolver>>>,
}

#[jsg_resource]
impl AsyncCloser {
    #[jsg_async_dispose]
    fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let resolver = v8::PromiseResolver::new(lock)?;
        *self.resolver.borrow_mut(lock) = Some(resolver.to_traced_global(lock));
        Ok(resolver.promise())
    }

    #[jsg_method]
    fn finish(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
        let resolver = self.take_resolver(lock)?;
        let undefined = lock.undefined();
        resolver.resolve(lock, undefined)
    }

    #[jsg_method]
    fn fail(&self, lock: &mut jsg::Lock<'_>, message: String) -> jsg::Result<()> {
        let resolver = self.take_resolver(lock)?;
        resolver.reject_with_error(lock, &jsg::Error::new_range_error(message))
    }
}

impl AsyncCloser {
    fn take_resolver<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::PromiseResolver>> {
        let resolver = self.resolver.borrow_mut(lock).take();
        resolver
            .map(|resolver| resolver.as_local(lock))
            .ok_or_else(|| jsg::Error::new_type_error("not disposing"))
    }
}

/// Runs an `await using` of `closer` in an async function, logging the scope's body, the code
/// after the scope, and what the function rejects with.
const AWAIT_USING: &str = "globalThis.log = [];
    (async () => {
      { await using c = closer; log.push('body'); }
      log.push('after');
    })().catch((e) => log.push(`${e.name}: ${e.message}`));";

#[test]
fn await_using_awaits_the_disposer_promise() {
    Harness::new().run_in_context(|lock, ctx| {
        let closer = jsg::Rc::new(
            lock,
            AsyncCloser {
                resolver: jsg::RefCell::new(None),
            },
        );
        ctx.set_global(lock, "closer", closer);
        assert_eq!(log_after_microtasks(lock, ctx, AWAIT_USING), "body");
        assert_eq!(
            log_after_microtasks(lock, ctx, "closer.finish()"),
            "body|after"
        );

        assert_eq!(log_after_microtasks(lock, ctx, AWAIT_USING), "body");
        assert_eq!(
            log_after_microtasks(lock, ctx, "closer.fail('nope')"),
            "body|RangeError: nope"
        );

        let result: String = ctx
            .eval(
                lock,
                "const proto = Object.getPrototypeOf(closer);
             const d = Object.getOwnPropertyDescriptor(proto, Symbol.asyncDispose);
             [typeof d.value, d.enumerable, Symbol.dispose in closer].join()",
            )
            .unwrap();
        assert_eq!(result, "function,false,false");
        Ok(())
    });
}

/// Disposes asynchronously through a typed promise.
#[jsg_resource]
struct TypedAsyncCloser {
    disposals: Cell<u32>,
}

#[jsg_resource]
impl TypedAsyncCloser {
    #[jsg_async_dispose]
    fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, u32>> {
        self.disposals.set(self.disposals.get() + 1);
        jsg::Promise::resolved(lock, self.disposals.get())
    }
}

#[test]
fn async_disposer_can_return_a_typed_promise() {
    Harness::new().run_in_context(|lock, ctx| {
        let closer = jsg::Rc::new(
            lock,
            TypedAsyncCloser {
                disposals: Cell::new(0),
            },
        );
        ctx.set_global(lock, "closer", closer);
        let code = "globalThis.log = [];
            (async () => {
              { await using c = closer; log.push('body'); }
              log.push(await closer[Symbol.asyncDispose]());
            })();";
        assert_eq!(log_after_microtasks(lock, ctx, code), "body|2");
        Ok(())
    });
}

/// Chooses its disposer by compatibility flag, through custom registration.
#[jsg_resource]
struct Chooser;

#[jsg_resource(custom_members)]
impl Chooser {
    #[jsg_dispose]
    fn old_dispose(&self) -> String {
        "old".to_owned()
    }

    #[jsg_async_dispose]
    fn new_dispose<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, String>> {
        jsg::Promise::resolved(lock, "new".to_owned())
    }
}

impl Resource for Chooser {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        if flags.get_node_js_compat() {
            registry.add(Self::new_dispose_async_dispose_member());
        } else {
            registry.add(Self::old_dispose_dispose_member());
        }
    }
}

fn chosen_disposer(harness: &Harness) -> String {
    let mut result = String::new();
    harness.run_in_context(|lock, ctx| {
        let chooser = jsg::Rc::new(lock, Chooser);
        ctx.set_global(lock, "chooser", chooser);
        let code = "globalThis.log = [];
            log.push(Symbol.dispose in chooser, Symbol.asyncDispose in chooser);
            (async () => {
              log.push(await (chooser[Symbol.dispose] ?? chooser[Symbol.asyncDispose])
                .call(chooser));
            })();";
        result = log_after_microtasks(lock, ctx, code);
        Ok(())
    });
    result
}

#[test]
fn custom_members_choose_a_disposer() {
    assert_eq!(chosen_disposer(&Harness::new()), "true|false|old");
    let harness = Harness::with_flags(|mut f| f.set_node_js_compat(true));
    assert_eq!(chosen_disposer(&harness), "false|true|new");
}

/// Has one disposer of each kind, as C++ JSG allows, and counts the calls of each.
#[jsg_resource]
struct Both {
    sync_disposals: Cell<u32>,
    async_disposals: Cell<u32>,
}

#[jsg_resource]
impl Both {
    #[jsg_dispose]
    fn dispose(&self) {
        self.sync_disposals.set(self.sync_disposals.get() + 1);
    }

    #[jsg_async_dispose]
    fn dispose_async<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        self.async_disposals.set(self.async_disposals.get() + 1);
        jsg::Promise::resolved(lock, ())
    }

    #[jsg_property(readonly)]
    fn get_disposals(&self) -> String {
        format!(
            "{}/{}",
            self.sync_disposals.get(),
            self.async_disposals.get()
        )
    }
}

#[test]
fn using_calls_the_sync_disposer_and_await_using_the_async_one() {
    Harness::new().run_in_context(|lock, ctx| {
        let both = jsg::Rc::new(
            lock,
            Both {
                sync_disposals: Cell::new(0),
                async_disposals: Cell::new(0),
            },
        );
        ctx.set_global(lock, "both", both);
        let log = log_after_microtasks(
            lock,
            ctx,
            "globalThis.log = [];
             { using b = both; }
             log.push(both.disposals);
             (async () => {
               { await using b = both; }
               log.push(both.disposals);
             })();",
        );
        assert_eq!(log, "1/0|1/1");
        Ok(())
    });
}

#[jsg_resource]
struct BothCustom;

#[jsg_resource(custom_members)]
impl BothCustom {
    #[jsg_dispose]
    fn dispose(&self) {}

    #[jsg_async_dispose]
    fn dispose_async<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        jsg::Promise::resolved(lock, ())
    }
}

impl Resource for BothCustom {
    fn members(registry: &mut impl MemberRegistry, _flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::dispose_dispose_member());
        registry.add(Self::dispose_async_async_dispose_member());
    }
}

#[test]
fn custom_members_may_register_both_disposers() {
    Harness::new().run_in_context(|lock, ctx| {
        let both = jsg::Rc::new(lock, BothCustom);
        ctx.set_global(lock, "both", both);
        let kinds: String = ctx
            .eval(
                lock,
                "`${typeof both[Symbol.dispose]}|${typeof both[Symbol.asyncDispose]}`",
            )
            .unwrap();
        assert_eq!(kinds, "function|function");
        Ok(())
    });
}

/// Fails before it has a promise to return.
#[jsg_resource]
struct FailingAsyncCloser;

#[jsg_resource]
impl FailingAsyncCloser {
    #[jsg_async_dispose]
    fn close<'s>(&self, _lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        Err(jsg::Error::new_range_error("cannot close"))
    }
}

#[test]
fn await_using_rejects_with_a_synchronous_async_disposer_error() {
    Harness::new().run_in_context(|lock, ctx| {
        let closer = jsg::Rc::new(lock, FailingAsyncCloser);
        ctx.set_global(lock, "closer", closer);
        assert_eq!(
            log_after_microtasks(lock, ctx, AWAIT_USING),
            "body|RangeError: cannot close"
        );
        Ok(())
    });
}

#[jsg_resource]
struct TwoDisposers;

#[jsg_resource(custom_members)]
impl TwoDisposers {
    #[jsg_dispose]
    fn first(&self) {}

    #[jsg_dispose]
    fn second(&self) {}
}

impl Resource for TwoDisposers {
    fn members(registry: &mut impl MemberRegistry, _flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::first_dispose_member());
        registry.add(Self::second_dispose_member());
    }
}

#[test]
#[should_panic(expected = "TwoDisposers has more than one dispose method")]
fn custom_members_reject_two_disposers_of_a_kind() {
    Harness::new().run_in_context(|lock, _ctx| {
        jsg::resource::function_template_of::<TwoDisposers>(lock).unwrap();
        Ok(())
    });
}
