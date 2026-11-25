// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The natives of the `utils` pseudo-global that need nothing but V8: type checks, symbols,
//! promise reactions and lazy properties. The rest forward to the C++ API layer, and C++ makes
//! them (`ScriptHost::api_natives()`); these are added to the object it returns.
//!
//! Each is a plain function without data, made before the context is otherwise set up, so none
//! relies on JSG's type wrappers.

use jsg::Error;
use jsg::Lock;
use jsg::fast_api;
use jsg::v8;
use jsg::v8::Function;
use jsg::v8::Local;
use jsg::v8::Object;
use jsg::v8::TracedGlobal;
use jsg::v8::Value;
use jsg::v8::ffi::SideEffectType;

use crate::Scope;
use crate::normalize;

fn is_array_buffer(value: Local<'_, Value>) -> bool {
    value.is_array_buffer()
}

fn is_array_buffer_view(value: Local<'_, Value>) -> bool {
    value.is_array_buffer_view()
}

fn is_data_view(value: Local<'_, Value>) -> bool {
    value.is_data_view()
}

fn is_promise(value: Local<'_, Value>) -> bool {
    value.is_promise()
}

fn is_shared_array_buffer(value: Local<'_, Value>) -> bool {
    value.is_shared_array_buffer()
}

fn is_uint8_array(value: Local<'_, Value>) -> bool {
    value.is_uint8_array()
}

fn is_any_array_buffer(value: Local<'_, Value>) -> bool {
    value.is_array_buffer() || value.is_shared_array_buffer()
}

fn mark_promise_handled(value: Local<'_, Value>) {
    if let Ok(promise) = Local::<v8::Promise>::try_from(value) {
        promise.mark_as_handled();
    }
}

/// Sets `name` on `utils` to a native function.
fn set_native<'s>(
    lock: &mut Lock<'s>,
    utils: Local<'s, Object>,
    name: &str,
    callback: jsg::RawFunctionCallback,
    fast: Option<fast_api::FastCallback>,
    side_effect: SideEffectType,
) -> jsg::Result<()> {
    let function = v8::FunctionTemplate::new(
        lock,
        callback,
        v8::ffi::ConstructorBehavior::Throw,
        side_effect,
        fast,
    )
    .get_function(lock)?;
    utils.set(lock, name, function.into())
}

/// Runs `f`, which computes a native's result, and returns the result or throws its error.
fn lift<'s>(
    lock: &mut Lock<'s>,
    args: &mut v8::FunctionCallbackInfo<'s>,
    f: impl FnOnce(&mut Lock<'s>, &v8::FunctionCallbackInfo<'s>) -> jsg::Result<Local<'s, Value>>,
) {
    match f(lock, args) {
        Ok(result) => args.set_return_value(result),
        Err(error) => lock.throw_exception(&error),
    }
}

fn string_arg<'s>(
    args: &v8::FunctionCallbackInfo<'s>,
    message: &'static str,
) -> jsg::Result<Local<'s, v8::String>> {
    Local::<v8::String>::try_from(args.get(0)).map_err(|_| Error::new_type_error(message))
}

/// Adds the natives this module implements to `utils`.
pub fn add_natives<'s>(lock: &mut Lock<'s>, utils: Local<'s, Object>) -> jsg::Result<()> {
    // A type check is fast and has no side effects, so the debugger can call it.
    macro_rules! type_check {
        ($name:literal, $check:ident) => {
            set_native(
                lock,
                utils,
                $name,
                jsg::function_callback(|_lock, args| {
                    let value = args.get(0);
                    args.set_return_boolean($check(value));
                }),
                Some(fast_api::function($check)),
                SideEffectType::HasNoSideEffect,
            )?;
        };
    }
    type_check!("isArrayBuffer", is_array_buffer);
    type_check!("isArrayBufferView", is_array_buffer_view);
    type_check!("isDataView", is_data_view);
    type_check!("isPromise", is_promise);
    type_check!("isSharedArrayBuffer", is_shared_array_buffer);
    type_check!("isUint8Array", is_uint8_array);
    type_check!("isAnyArrayBuffer", is_any_array_buffer);

    add_promise_natives(lock, utils)?;
    add_string_natives(lock, utils)?;
    add_lazy_data_property(lock, utils)
}

fn add_promise_natives<'s>(lock: &mut Lock<'s>, utils: Local<'s, Object>) -> jsg::Result<()> {
    set_native(
        lock,
        utils,
        "markPromiseHandled",
        jsg::function_callback(|_lock, args| mark_promise_handled(args.get(0))),
        Some(fast_api::function(mark_promise_handled)),
        SideEffectType::HasSideEffect,
    )?;

    // Calls the function argument, with no arguments and an undefined receiver, as a reaction
    // to an already-resolved promise. `Local<Promise>::then` neither reads `then` nor looks up
    // the species, so scheduling runs no user code. If the function throws, the reaction's
    // promise, which is dropped, rejects, and the rejection is reported as unhandled.
    set_native(
        lock,
        utils,
        "queueReaction",
        jsg::function_callback(|lock, args| {
            lift(lock, args, |lock, args| {
                let function = Local::<Function>::try_from(args.get(0))
                    .map_err(|_| Error::new_type_error("queueReaction() expects a function"))?;
                let function = TracedGlobal::new(lock, function);
                let undefined = lock.undefined();
                let resolved = v8::Promise::resolved(lock, undefined)?;
                resolved.then(lock, function, |lock, function, _value| {
                    let function = function.as_local(lock);
                    function.call(lock, None, &[])
                })?;
                Ok(lock.undefined())
            });
        }),
        None,
        SideEffectType::HasSideEffect,
    )
}

fn add_string_natives<'s>(lock: &mut Lock<'s>, utils: Local<'s, Object>) -> jsg::Result<()> {
    // The symbol of the isolate's API registry for a name, which C++ reaches by the same name.
    set_native(
        lock,
        utils,
        "getApiSymbol",
        jsg::function_callback(|lock, args| {
            lift(lock, args, |lock, args| {
                let name = string_arg(args, "getApiSymbol() expects a string argument")?;
                Ok(v8::Symbol::for_api(lock, name).into())
            });
        }),
        None,
        SideEffectType::HasSideEffect,
    )?;

    // The V8 private symbol of a name, as a property key; see `Local::<Private>::to_property_key`.
    set_native(
        lock,
        utils,
        "getPrivateSymbol",
        jsg::function_callback(|lock, args| {
            lift(lock, args, |lock, args| {
                let name = string_arg(args, "getPrivateSymbol() expects a string argument")?;
                v8::Private::for_api(lock, name).to_property_key(lock)
            });
        }),
        None,
        SideEffectType::HasSideEffect,
    )?;

    // The exact UTF-8 byte length of a string as TextEncoder encodes it: a lone surrogate counts
    // the 3 bytes of U+FFFD. Not a fast call: V8 flattens the string first, which can allocate.
    set_native(
        lock,
        utils,
        "utf8Length",
        jsg::function_callback(|lock, args| {
            match string_arg(args, "utf8Length() expects a string") {
                #[expect(
                    clippy::cast_precision_loss,
                    reason = "a string's UTF-8 length is far below 2^53"
                )]
                Ok(string) => args.set_return_number(string.utf8_length(lock) as f64),
                Err(error) => lock.throw_exception(&error),
            }
        }),
        None,
        SideEffectType::HasSideEffect,
    )?;

    Ok(())
}

fn add_lazy_data_property<'s>(lock: &mut Lock<'s>, utils: Local<'s, Object>) -> jsg::Result<()> {
    // Defines the property `key` (a string or symbol) of `target` as a writable, configurable
    // data property, enumerable if `enumerable` is true, whose value is the `key` export of the
    // bootstrap module `specifier`, which is required when the property is first read; reading
    // its descriptor counts. Until then nothing is loaded, and nothing but the timing of the
    // load distinguishes it from an ordinary data property: it is V8's lazy data property, as
    // JSG_LAZY_INSTANCE_PROPERTY defines. A read that throws leaves the property lazy, so the
    // next read tries again.
    set_native(
        lock,
        utils,
        "defineLazyDataProperty",
        jsg::function_callback(|lock, args| {
            lift(lock, args, |lock, args| {
                let target = Local::<Object>::try_from(args.get(0)).map_err(|_| {
                    Error::new_type_error("defineLazyDataProperty() expects an object")
                })?;
                let key = Local::<v8::Name>::try_from(args.get(1)).map_err(|_| {
                    Error::new_type_error("defineLazyDataProperty() expects a property key")
                })?;
                // A property still lazy when a zygote takes its snapshot keeps the getter and
                // its data in the isolate's snapshot, which every context restored from it
                // shares, so the data is the specifier rather than anything of this context's,
                // and the getter requires the module in the context it is read in.
                let specifier = args.get(2);
                if !specifier.is_string() {
                    return Err(Error::new_type_error(
                        "defineLazyDataProperty() expects a module specifier",
                    ));
                }
                let enumerable = Local::<v8::Boolean>::try_from(args.get(3))
                    .is_ok_and(|enumerable| enumerable.value());
                let attributes = if enumerable {
                    v8::PropertyAttribute::NONE
                } else {
                    v8::PropertyAttribute::DONT_ENUM
                };
                target.set_lazy_data_property(lock, key, lazy_getter(), specifier, attributes)?;
                Ok(lock.undefined())
            });
        }),
        None,
        SideEffectType::HasSideEffect,
    )
}

/// The getter of a property that `defineLazyDataProperty()` defined: requires the bootstrap
/// module named by the property's data and returns its export of the property's name. V8 then
/// replaces the property with a data property holding the result.
fn lazy_getter() -> jsg::RawGetterCallback {
    jsg::accessor_name_getter_callback(|lock, name, info| match lazy_value(lock, name, info) {
        Ok(value) => info.set_return_value(value),
        Err(error) => lock.throw_exception(&error),
    })
}

fn lazy_value<'s>(
    lock: &mut Lock<'s>,
    name: Local<'s, v8::Name>,
    info: &v8::PropertyCallbackInfo<'s>,
) -> jsg::Result<Local<'s, Value>> {
    let specifier = Local::<v8::String>::try_from(info.data())
        .map_err(|_| Error::new_internal_error("lazy data property without a module specifier"))?
        .to_string(lock);
    let specifier = normalize(&specifier);
    let state = crate::state(lock)?;
    let exports = match state.loaded(lock, specifier) {
        Some(exports) => exports,
        None => state.require(lock, specifier, Scope::Workerd)?,
    };
    let exports = Local::<Object>::try_from(exports).map_err(|_| {
        Error::new_internal_error(format!(
            "bootstrap module's exports are not an object: {specifier}"
        ))
    })?;
    exports.get_by_name(lock, name)
}
