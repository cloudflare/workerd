// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Local<Value>` converts like C++ JSG's `v8::Local<v8::Value>`: any value passes through
//! unchanged in both directions.

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Values {
    stored: jsg::RefCell<Option<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl Values {
    #[jsg_method]
    pub fn identity<'s>(&self, value: v8::Local<'s, v8::Value>) -> v8::Local<'s, v8::Value> {
        value
    }

    #[jsg_method]
    pub fn optional<'s>(&self, value: Option<v8::Local<'s, v8::Value>>) -> String {
        value.map_or_else(|| "none".to_owned(), |_| "some".to_owned())
    }

    #[jsg_method]
    pub fn store<'s>(&self, lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
        let value = value.to_traced_global(lock);
        *self.stored.borrow_mut(lock) = Some(value);
    }

    #[jsg_method]
    pub fn load<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> Result<v8::Local<'s, v8::Value>, jsg::Error> {
        self.stored
            .borrow()
            .as_ref()
            .map(|global| global.as_local(lock))
            .ok_or_else(|| jsg::Error::new_error("nothing stored"))
    }
}

fn install(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let values = jsg::Rc::new(Values {
        stored: jsg::RefCell::new(None),
    })
    .to_js(lock);
    ctx.set_global(lock, "values", values);
}

#[test]
fn conversions_return_the_same_handle() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let object = ctx.eval_raw(lock, "({})").unwrap();
        assert_eq!(
            v8::Local::<v8::Value>::from_js(lock, object).unwrap(),
            object
        );
        assert_eq!(object.to_js(lock), object);
        Ok(())
    });
}

#[test]
fn method_passes_any_value_through() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let code = "
            const object = {};
            const fn = () => {};
            const sym = Symbol('s');
            [object, fn, sym, 1, 'str', null, undefined, 10n, true]
                .every((v) => Object.is(values.identity(v), v))
        ";
        assert!(ctx.eval::<bool>(lock, code).unwrap());
        Ok(())
    });
}

#[test]
fn method_does_not_coerce() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let code = "
            const hostile = {
                toString() { throw new Error('toString'); },
                valueOf() { throw new Error('valueOf'); },
                [Symbol.toPrimitive]() { throw new Error('toPrimitive'); },
            };
            values.identity(hostile) === hostile
        ";
        assert!(ctx.eval::<bool>(lock, code).unwrap());
        Ok(())
    });
}

#[test]
fn missing_argument_matches_cpp_jsg() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let code = "try { values.identity(); 'ok' } catch (e) { `${e.name}: ${e.message}` }";
        assert_eq!(
            ctx.eval::<String>(lock, code).unwrap(),
            "TypeError: Failed to execute 'identity' on 'Values': parameter 1 is not of type \
             'Value'."
        );
        Ok(())
    });
}

#[test]
fn option_reads_undefined_as_none() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let code = "[values.optional(), values.optional(undefined), values.optional(null), \
                    values.optional(0)].join()";
        assert_eq!(
            ctx.eval::<String>(lock, code).unwrap(),
            "none,none,some,some"
        );
        Ok(())
    });
}

#[test]
fn stored_value_round_trips_through_traced_global() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let code = "
            const stored = { key: 1 };
            values.store(stored);
            values.load() === stored
        ";
        assert!(ctx.eval::<bool>(lock, code).unwrap());
        Ok(())
    });
}
