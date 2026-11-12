// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "unwraps DOMExceptions and creates errors through the V8 FFI"
)]

//! JavaScript exceptions tunnelled through a `kj::Exception`, as C++ JSG tunnels them.

use crate::Error;
use crate::FromJS;
use crate::Lock;
use crate::Result;
use crate::ToJS;
use crate::ser;
use crate::v8;
use crate::v8::JsErrorType;

/// A JavaScript exception as C++ JSG converts it to a `kj::Exception`, with
/// `jsg::Lock::exceptionToKj()`, holding what `jsg::Lock::exceptionToJs()` reads to convert it
/// back.
///
/// C++ code that catches a JavaScript exception and throws it on as a `kj::Exception` hands
/// JavaScript a copy of the exception, not the exception itself: what
/// [`TunneledException::from_js`] and then [`TunneledException::to_js`] return. Depending on the
/// exception and the `enhanced_error_serialization` compatibility flag, the copy is:
///
/// - A `DOMException` with the name and message of a `DOMException`.
/// - With the flag on, the exception as deserialized from what serializing it wrote, without
///   its stack, if that is an object.
/// - Otherwise, an error of the type that the start of the exception's detail string names,
///   with the message that follows it. An exception whose detail string names none of the types
///   that C++ JSG tunnels becomes an `Error` with the detail string as its message.
///
/// A copy of an exception with a truthy `overloaded` property gets `overloaded = true`, and
/// otherwise one with a truthy `retryable` property gets `retryable = true`.
///
/// The `kj::Exception` that C++ JSG creates also holds the exception's name and stack, which
/// `exceptionToJs()` does not read, so this does not hold them.
#[derive(Debug, Clone)]
pub struct TunneledException {
    kind: Kind,
    /// The description of the `kj::Exception`, such as `jsg.TypeError: message`.
    description: String,
    /// What serializing the exception wrote, if it could be serialized.
    detail: Option<Vec<u8>>,
    /// Whether the `enhanced_error_serialization` compatibility flag was on when `detail` was
    /// written: only then may `to_js` read it.
    detail_drops_stacks: bool,
}

/// The type of the `kj::Exception`, which C++ JSG takes from the exception's properties.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Kind {
    Failed,
    Overloaded,
    Disconnected,
}

/// The error types that C++ JSG tunnels, `JS_ERROR_TYPES` in `jsg/util.h`, in its order, under
/// the names that start their descriptions.
const JS_ERROR_TYPES: [(&str, JsErrorType); 13] = [
    ("Error", JsErrorType::Error),
    ("RangeError", JsErrorType::RangeError),
    ("TypeError", JsErrorType::TypeError),
    ("SyntaxError", JsErrorType::SyntaxError),
    ("ReferenceError", JsErrorType::ReferenceError),
    ("CompileError", JsErrorType::WasmCompileError),
    ("LinkError", JsErrorType::WasmLinkError),
    ("RuntimeError", JsErrorType::WasmRuntimeError),
    ("SuspendError", JsErrorType::WasmSuspendError),
    ("AggregateError", JsErrorType::AggregateError),
    ("SuppressedError", JsErrorType::SuppressedError),
    ("URIError", JsErrorType::URIError),
    ("EvalError", JsErrorType::EvalError),
];

/// The prefix of a description that C++ JSG converts back to a JavaScript exception.
const JSG_PREFIX: &str = "jsg.";

impl TunneledException {
    /// Converts `exception` as C++ JSG's `ExceptionWrapper` converts it to a `kj::Exception`.
    ///
    /// This reads the exception's `overloaded`, `retryable`, `name` and `stack` properties and
    /// serializes it, all of which can run JavaScript. What serializing it throws is caught, and
    /// the exception is then not serialized.
    ///
    /// # Errors
    ///
    /// Fails, leaving the exception pending, if reading a property throws, or if execution is
    /// terminating; see [`Error::is_pending`].
    pub fn from_js<'s>(lock: &mut Lock<'s>, exception: v8::Local<'s, v8::Value>) -> Result<Self> {
        let object = v8::Local::<v8::Object>::try_from(exception).ok();
        let kind = match object {
            Some(object) if truthy(lock, object, "overloaded")? => Kind::Overloaded,
            Some(object) if truthy(lock, object, "retryable")? => Kind::Disconnected,
            _ => Kind::Failed,
        };
        let description = if let Some((name, message)) = try_unwrap_dom_exception(lock, exception)?
        {
            format!("{JSG_PREFIX}DOMException({name}): {message}")
        } else {
            let reason = exception.to_detail_string(lock)?;
            let tunneled = object.is_some()
                && JS_ERROR_TYPES
                    .iter()
                    .map(|(name, _)| *name)
                    .chain(["DOMException"])
                    .any(|name| reason.starts_with(name));
            if tunneled {
                format!("{JSG_PREFIX}{reason}")
            } else {
                format!("{JSG_PREFIX}Error: {reason}")
            }
        };
        // C++ JSG writes the detail at version 15, which V8 currently writes anyway; it is only
        // read back by this isolate.
        let detail = lock.try_catch(|lock| ser::serialize(lock, exception))?.ok();
        let detail_drops_stacks = lock.feature_flags().get_enhanced_error_serialization();
        if let Some(object) = object {
            // C++ JSG reads these for the exception's metadata, which runs their getters.
            object.get(lock, "name")?;
            object.get(lock, "stack")?;
        }
        Ok(Self {
            kind,
            description,
            detail,
            detail_drops_stacks,
        })
    }

    /// Converts this back to a JavaScript exception, as `jsg::Lock::exceptionToJs()` converts a
    /// `kj::Exception` with its default options. Runs no JavaScript, except in setters of
    /// `overloaded` or `retryable` on the copy's prototype chain.
    ///
    /// # Errors
    ///
    /// Fails, leaving the exception pending, if creating a `DOMException` or setting a property
    /// throws, or if execution is terminating; see [`Error::is_pending`].
    pub fn to_js<'s>(&self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>> {
        if let Some(error_type) = self.description.strip_prefix(JSG_PREFIX)
            && let Some(exception) = self.decode(lock, error_type)?
        {
            self.add_additional_info(lock, exception)?;
            return Ok(exception);
        }
        // Not a JavaScript error type that C++ JSG recognizes.
        if self.kind == Kind::Disconnected {
            let exception = js_error(lock, JsErrorType::Error, "Network connection lost.");
            set_true(lock, exception, "retryable")?;
            return Ok(exception.into());
        }
        let exception = lock.exception(&Error::new_internal_error(self.description.clone()))?;
        self.add_additional_info(lock, exception)?;
        Ok(exception)
    }

    /// The exception that the description after its `jsg.` prefix, `error_type`, describes, if
    /// it describes one.
    fn decode<'s>(
        &self,
        lock: &mut Lock<'s>,
        error_type: &str,
    ) -> Result<Option<v8::Local<'s, v8::Value>>> {
        if let Some(rest) = error_type.strip_prefix("DOMException(")
            && let Some((name, message)) = rest.split_once(')')
        {
            let exception = lock.dom_exception(name, trim_error_message(message))?;
            return Ok(Some(exception.into()));
        }
        if self.detail_drops_stacks
            && lock.feature_flags().get_enhanced_error_serialization()
            && let Some(detail) = &self.detail
        {
            let options = ser::DeserializeOptions {
                preserve_stack_in_errors: false,
            };
            // What deserializing it throws is ignored.
            if let Ok(exception) =
                lock.try_catch(|lock| ser::deserialize_with(lock, detail, options))?
                && exception.is_object()
            {
                return Ok(Some(exception));
            }
        }
        Ok(JS_ERROR_TYPES.iter().find_map(|&(name, ty)| {
            let message = error_type.strip_prefix(name)?;
            Some(js_error(lock, ty, trim_error_message(message)).into())
        }))
    }

    /// Sets the properties that the `kj::Exception`'s type calls for on `exception`, if it is an
    /// object.
    fn add_additional_info<'s>(
        &self,
        lock: &mut Lock<'s>,
        exception: v8::Local<'s, v8::Value>,
    ) -> Result<()> {
        let Ok(exception) = v8::Local::<v8::Object>::try_from(exception) else {
            return Ok(());
        };
        match self.kind {
            Kind::Failed => Ok(()),
            Kind::Disconnected => set_true(lock, exception, "retryable"),
            Kind::Overloaded => set_true(lock, exception, "overloaded"),
        }
    }
}

/// Whether `object[key]` is truthy.
fn truthy<'s>(lock: &mut Lock<'s>, object: v8::Local<'s, v8::Object>, key: &str) -> Result<bool> {
    let value = object.get(lock, key)?;
    bool::from_js(lock, value)
}

fn set_true<'s>(lock: &mut Lock<'s>, object: v8::Local<'s, v8::Object>, key: &str) -> Result<()> {
    let value = true.to_js(lock)?;
    object.set(lock, key, value)
}

/// The message in what follows an error type in a description: everything after a `:` and the
/// spaces that follow it, or nothing if it does not start with a `:`.
fn trim_error_message(rest: &str) -> &str {
    rest.strip_prefix(':')
        .map_or("", |message| message.trim_start_matches(' '))
}

/// An error of `ty` with `message`.
fn js_error<'s>(lock: &mut Lock<'s>, ty: JsErrorType, message: &str) -> v8::Local<'s, v8::Object> {
    let message = crate::error_message_for_v8(message.as_bytes());
    // SAFETY: the lock guarantees the isolate is valid and locked. V8's error constructors
    // always return an object.
    unsafe {
        v8::Local::from_ffi(v8::ffi::js_error_create_from_bytes(
            lock.isolate().as_ffi(),
            ty,
            message,
        ))
    }
}

/// The name and message of the `DOMException` that `value` unwraps as, as C++ JSG unwraps a
/// `DOMException&` parameter: `value` or the first object in its prototype chain that is one.
fn try_unwrap_dom_exception<'s>(
    lock: &mut Lock<'s>,
    value: v8::Local<'s, v8::Value>,
) -> Result<Option<(String, String)>> {
    let mut name = String::new();
    let mut message = String::new();
    // SAFETY: the lock guarantees the isolate is valid and locked, with a context entered;
    // `value` belongs to its scope.
    let is_dom_exception = unsafe {
        v8::ffi::dom_exception_try_unwrap(
            lock.isolate().as_ffi(),
            value.raw(),
            &mut name,
            &mut message,
        )
    }?;
    Ok(is_dom_exception.then_some((name, message)))
}
