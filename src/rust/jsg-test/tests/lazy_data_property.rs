// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Local<Object>::set_lazy_data_property` (C++ `v8::Object::SetLazyDataProperty()`) with a
//! getter from `jsg::accessor_name_getter_callback`, which gets the property's name and
//! `PropertyCallbackInfo::data`, and `Local<Object>::delete` (C++ `v8::Object::Delete()`).

use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg::v8::PropertyAttribute;

/// How often [`describe`] ran. The tests run serially, so each reads it before and after.
static GETTER_CALLS: AtomicUsize = AtomicUsize::new(0);
/// Makes [`describe`] throw.
static GETTER_THROWS: AtomicBool = AtomicBool::new(false);

/// A getter that returns `"<data>:<name>"`.
fn describe() -> jsg::RawGetterCallback {
    jsg::accessor_name_getter_callback(|lock, name, info| {
        GETTER_CALLS.fetch_add(1, Ordering::SeqCst);
        if GETTER_THROWS.load(Ordering::SeqCst) {
            lock.throw_exception(&jsg::Error::new_range_error("not yet"));
            return;
        }
        let name = v8::Local::<v8::Value>::from(name);
        let name = match v8::Local::<v8::String>::try_from(name) {
            Ok(name) => name.to_string(lock),
            Err(_) => "symbol".to_owned(),
        };
        let data = v8::Local::<v8::String>::try_from(info.data())
            .unwrap()
            .to_string(lock);
        let value = format!("{data}:{name}").to_js(lock).unwrap();
        info.set_return_value(value);
    })
}

fn object<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    code: &str,
) -> v8::Local<'s, v8::Object> {
    v8::Local::<v8::Object>::try_from(ctx.eval_raw(lock, code).unwrap()).unwrap()
}

fn name<'s>(lock: &mut jsg::Lock<'s>, name: &str) -> v8::Local<'s, v8::Name> {
    v8::String::new_internalized_from_str(lock, name)
        .unwrap()
        .into()
}

/// Defines the lazy property `key` of the global `o`, a new object, with data `data`.
fn define_on_o(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    key: &str,
    data: &str,
    attributes: PropertyAttribute,
) -> jsg::Result<()> {
    let o = object(lock, ctx, "globalThis.o = {}");
    let key = name(lock, key);
    let data = data.to_js(lock)?;
    o.set_lazy_data_property(lock, key, describe(), data, attributes)
}

#[test]
fn first_read_computes_the_value_once() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define_on_o(lock, ctx, "k", "data", PropertyAttribute::NONE)?;
        let before = GETTER_CALLS.load(Ordering::SeqCst);
        assert_eq!(
            ctx.eval::<String>(lock, "o.k + '|' + o.k").unwrap(),
            "data:k|data:k"
        );
        assert_eq!(GETTER_CALLS.load(Ordering::SeqCst) - before, 1);
        // Then an ordinary writable, configurable data property.
        assert!(
            ctx.eval::<bool>(
                lock,
                "const d = Object.getOwnPropertyDescriptor(o, 'k');
                 d.value === 'data:k' && d.writable && d.configurable && !('get' in d)",
            )
            .unwrap()
        );
        Ok(())
    });
}

#[test]
fn attributes_apply_to_the_value_once_read() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define_on_o(
            lock,
            ctx,
            "k",
            "d",
            PropertyAttribute::READ_ONLY | PropertyAttribute::DONT_DELETE,
        )?;
        assert!(
            ctx.eval::<bool>(
                lock,
                "o.k;
                 const d = Object.getOwnPropertyDescriptor(o, 'k');
                 !d.writable && !d.configurable && d.enumerable",
            )
            .unwrap()
        );
        Ok(())
    });
}

#[test]
fn reading_the_descriptor_counts_as_a_read() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define_on_o(lock, ctx, "k", "d", PropertyAttribute::NONE)?;
        let before = GETTER_CALLS.load(Ordering::SeqCst);
        assert_eq!(
            ctx.eval::<String>(lock, "Object.getOwnPropertyDescriptor(o, 'k').value")
                .unwrap(),
            "d:k"
        );
        assert_eq!(GETTER_CALLS.load(Ordering::SeqCst) - before, 1);
        Ok(())
    });
}

#[test]
fn dont_enum_hides_it_from_keys() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define_on_o(lock, ctx, "shown", "d", PropertyAttribute::NONE)?;
        assert_eq!(
            ctx.eval::<String>(lock, "Object.keys(o).join()").unwrap(),
            "shown"
        );
        define_on_o(lock, ctx, "hidden", "d", PropertyAttribute::DONT_ENUM)?;
        assert_eq!(
            ctx.eval::<String>(lock, "Object.keys(o).length + ',' + ('hidden' in o)")
                .unwrap(),
            "0,true"
        );
        Ok(())
    });
}

#[test]
fn symbol_key_reaches_the_getter() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let o = object(lock, ctx, "globalThis.o = {}");
        let symbol = v8::Local::<v8::Name>::try_from(
            ctx.eval_raw(lock, "globalThis.s = Symbol('s')").unwrap(),
        )
        .unwrap();
        let data = "d".to_js(lock)?;
        o.set_lazy_data_property(lock, symbol, describe(), data, PropertyAttribute::NONE)?;
        assert_eq!(ctx.eval::<String>(lock, "o[s]").unwrap(), "d:symbol");
        Ok(())
    });
}

#[test]
fn read_that_throws_leaves_the_property_lazy() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define_on_o(lock, ctx, "k", "d", PropertyAttribute::NONE)?;
        GETTER_THROWS.store(true, Ordering::SeqCst);
        let first = ctx
            .eval::<String>(lock, "try { o.k } catch (e) { e.name }")
            .unwrap();
        GETTER_THROWS.store(false, Ordering::SeqCst);
        assert_eq!(first, "RangeError");
        assert_eq!(ctx.eval::<String>(lock, "o.k").unwrap(), "d:k");
        Ok(())
    });
}

#[test]
fn non_extensible_object_refuses_the_property() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let o = object(lock, ctx, "Object.preventExtensions({})");
        let key = name(lock, "k");
        let data = "d".to_js(lock)?;
        let err = o
            .set_lazy_data_property(lock, key, describe(), data, PropertyAttribute::NONE)
            .unwrap_err();
        assert!(!err.is_pending(), "pending: {err}");
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}

#[test]
fn lazy_property_survives_a_startup_snapshot() {
    crate::snapshot_round_trip(
        |lock, ctx| define_on_o(lock, ctx, "k", "zygote", PropertyAttribute::NONE),
        |lock, ctx| {
            assert_eq!(ctx.eval::<String>(lock, "o.k").unwrap(), "zygote:k");
            Ok(())
        },
    )
    .unwrap();
}

#[test]
fn delete_removes_a_configurable_property() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let o = object(lock, ctx, "globalThis.o = { a: 1 }");
        let a = name(lock, "a");
        assert!(o.delete(lock, a)?);
        let missing = name(lock, "missing");
        assert!(o.delete(lock, missing)?);
        assert!(ctx.eval::<bool>(lock, "!('a' in o)").unwrap());
        Ok(())
    });
}

#[test]
fn delete_keeps_a_non_configurable_property() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let o = object(
            lock,
            ctx,
            "globalThis.o = Object.defineProperty({}, 'a', { value: 1, configurable: false })",
        );
        let a = name(lock, "a");
        assert!(!o.delete(lock, a)?);
        assert!(ctx.eval::<bool>(lock, "o.a === 1").unwrap());
        Ok(())
    });
}

#[test]
fn delete_leaves_what_a_proxy_trap_throws_pending() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let o = object(
            lock,
            ctx,
            "new Proxy({}, { deleteProperty() { throw new RangeError('trap'); } })",
        );
        let caught = lock.try_catch(|lock| {
            let a = name(lock, "a");
            o.delete(lock, a)
        })?;
        let error = jsg::Error::from_js(lock, caught.expect_err("the trap throws"))?;
        assert_eq!(error.name, ExceptionType::RangeError);
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}
