// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A read that waits for the cache: as the leader, until it runs its fallback, or as a waiter,
//! until the read it waits for finishes.

use std::sync::Arc;

use io_context::IoContext;
use io_context::IoOwned;
use io_context::IoOwnedSpan;
use io_context::SpanBuilder;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_traced;
use memory_cache::CacheError;
use memory_cache::FallbackPermit;

use crate::NoData;
use crate::cache_error;
use crate::now_ms;
use crate::trace;
use crate::trace::ReadSpans;

/// What a read needs to finish once the cache lets it. `IoContext::await_io_and_run()` holds it
/// outside the garbage collector's view, so its handles are strong.
pub struct PendingRead {
    resolver: v8::Global<v8::PromiseResolver>,
    fallback: v8::Global<v8::Function>,
    /// The key as C++ holds it: UTF-8, with lone surrogates encoded as such.
    key: Vec<u8>,
    spans: ReadSpans,
}

impl PendingRead {
    /// A pending read, and the promise that it settles.
    pub fn new<'s>(
        lock: &mut jsg::Lock<'s>,
        fallback: v8::Local<'s, v8::Function>,
        key: Vec<u8>,
        spans: ReadSpans,
    ) -> jsg::Result<(Self, v8::Local<'s, v8::Promise>)> {
        let resolver = v8::PromiseResolver::new(lock)?;
        let promise = resolver.promise();
        let pending = Self {
            resolver: resolver.to_global(lock),
            fallback: fallback.to_global(lock),
            key,
            spans,
        };
        Ok((pending, promise))
    }

    /// Resolves the read with the value another read cached.
    pub fn resolve_with(
        mut self,
        lock: &mut jsg::Lock<'_>,
        value: Result<memory_cache::Value, CacheError>,
    ) {
        let resolver = self.resolver.as_local(lock);
        let value = match value {
            Ok(value) => value,
            Err(err) => return reject(lock, resolver, &cache_error(err)),
        };
        self.spans.read.set_tag(c"fallback_cache_hit", true);
        self.spans
            .read
            .set_tag(c"entry_size", trace::count(value.bytes().len()));
        match lock.try_catch(|lock| jsg::ser::deserialize(lock, value.bytes())) {
            // As memory-cache.c++ returns `js.resolvedPromise(value)` to awaitIo(), which resolves
            // the read's promise with it.
            Ok(Ok(value)) => {
                if let Ok(value) = v8::Promise::resolved(lock, value) {
                    let _ = resolver.resolve(lock, value.into());
                }
            }
            Ok(Err(exception)) => {
                let _ = resolver.reject(lock, exception);
            }
            // Execution is terminating.
            Err(_) => {}
        }
    }

    /// Rejects the read with `err`.
    pub fn reject(self, lock: &mut jsg::Lock<'_>, err: &jsg::Error) {
        let resolver = self.resolver.as_local(lock);
        reject(lock, resolver, err);
    }

    /// Runs the read's fallback, holding `permit` until it produces a value or fails, or the
    /// request ends.
    pub fn run_fallback(self, lock: &mut jsg::Lock<'_>, permit: FallbackPermit) {
        let Self {
            resolver,
            fallback,
            key,
            // The user span ends when the fallback starts, as memory-cache.c++'s does.
            spans: ReadSpans { read, user: _user },
        } = self;
        let resolver = resolver.as_local(lock);
        // As memory-cache.c++ passes it, decoded from the bytes it holds, so that a lone
        // surrogate reaches the fallback as U+FFFD.
        let Some(key_js) = v8::String::new_from_utf8(lock, &key) else {
            let err = jsg::Error::new_internal_error("the memory cache key is too long");
            return reject(lock, resolver, &err);
        };
        let fill = match Fill::new(lock, permit, &key, read) {
            Ok(fill) => SharedFill(Arc::new(fill)),
            Err(err) => return reject(lock, resolver, &err),
        };
        let fallback = fallback.as_local(lock);
        // With `globalThis` as `this`, as C++ JSG calls a `jsg::Function`.
        let global = lock.global_object();
        // As memory-cache.c++ calls the fallback in `js.evalNow()`: what it throws, or what
        // converting what it produced throws, rejects the promise.
        let produced = match lock.try_catch(|lock| {
            let produced = fallback.call(lock, Some(global.into()), &[key_js.into()])?;
            produce(lock, produced)
        }) {
            Ok(Ok(promise)) => Ok(promise),
            Ok(Err(exception)) => v8::Promise::rejected(lock, exception),
            // Execution is terminating.
            Err(_) => return,
        };
        // As memory-cache.c++ chains `.then()` and `.catch_()` to it, and returns the result to
        // awaitIo(), which resolves the read's promise with it.
        let filled = produced
            .and_then(|promise| promise.then(lock, SharedFill(Arc::clone(&fill.0)), fulfilled))
            .and_then(|promise| promise.catch(lock, fill, rejected));
        match filled {
            Ok(filled) => {
                let _ = resolver.resolve(lock, filled.into());
            }
            Err(err) => reject(lock, resolver, &err),
        }
    }
}

fn reject<'s>(
    lock: &mut jsg::Lock<'s>,
    resolver: v8::Local<'s, v8::PromiseResolver>,
    err: &jsg::Error,
) {
    // A pending error is a termination, which leaves the read pending.
    if !err.is_pending() {
        let _ = resolver.reject_with_error(lock, err);
    }
}

/// What the reactions to a fallback's promise hold.
///
/// The garbage collector drops the reactions, on whichever thread then locks the isolate, and
/// only when it collects them if the promise never settles. A span reports to its request's
/// observer, whose state belongs to the request's thread, so the request owns the spans, as in
/// memory-cache.c++: they end when the reactions drop them, or with the request.
#[jsg_traced]
struct Fill {
    permit: IoOwned<Permit>,
    /// The key, for the span of the write.
    key: String,
    /// Before its parent, so that it is released first, and ends first.
    fallback_span: IoOwnedSpan,
    _read_span: IoOwnedSpan,
}

/// The [`Fill`] that the reactions to the fallback's result share, as memory-cache.c++'s share
/// its callback and spans. It holds no JavaScript values, so sharing it is sound.
#[jsg_traced]
struct SharedFill(Arc<Fill>);

/// A fallback's permit, which holds no JavaScript values, for the request to own.
struct Permit(FallbackPermit);

jsg::jsg_traced!(Permit);

impl Fill {
    /// Gives the request `permit`, so that it is dropped with the request: a fallback that never
    /// settles then does not keep the reads waiting for it until the garbage collector drops its
    /// reactions. Starts the span of the fallback, and gives the request it and `read_span`.
    fn new(
        lock: &mut jsg::Lock<'_>,
        permit: FallbackPermit,
        key: &[u8],
        mut read_span: SpanBuilder,
    ) -> jsg::Result<Self> {
        let key = String::from_utf8_lossy(key).into_owned();
        let mut fallback_span = read_span.new_child(c"memory_cache_fallback");
        fallback_span.set_tag(c"key", &key);
        let context = IoContext::current(lock)?;
        // The parent first, so that the request, which destroys what it owns newest first,
        // ends the child first.
        let read_span = context.add_span(read_span)?;
        Ok(Self {
            permit: context.add_object(Permit(permit))?,
            key,
            fallback_span: context.add_span(fallback_span)?,
            _read_span: read_span,
        })
    }

    /// Fails, as memory-cache.c++ does when it reaches the spans, unless the request that ran
    /// the fallback is the current one.
    fn require_current(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
        self.fallback_span.with(lock, |_| ())?;
        Ok(())
    }

    /// Fails unless the request that ran the fallback is the current one.
    fn tag_fallback<'v>(
        &self,
        lock: &mut jsg::Lock<'_>,
        key: &'static std::ffi::CStr,
        value: impl Into<io_context::TagValue<'v>>,
    ) -> jsg::Result<()> {
        self.fallback_span
            .with(lock, |span| span.set_tag(key, value))?;
        Ok(())
    }

    /// Records why the fallback failed, and abandons it, so that a read waiting for it runs its
    /// own. Fails if execution is terminating, or, as memory-cache.c++ does, if the request that
    /// ran the fallback is not the current one.
    fn fail<'a>(
        &self,
        lock: &mut jsg::Lock<'a>,
        exception: v8::Local<'a, v8::Value>,
    ) -> jsg::Result<()> {
        self.tag_fallback(lock, c"fallback_success", false)?;
        let error = exception.to_detail_string(lock)?;
        self.tag_fallback(lock, c"fallback_error", &error)?;
        drop(self.permit.take(lock)?);
        Ok(())
    }

    /// Caches what the fallback produced, and records the write. Like memory-cache.c++, which
    /// only logs it, a failure to do either still passes the value to the reads waiting for it.
    /// Fails, as memory-cache.c++ does, if the request that ran the fallback is not the current
    /// one.
    fn succeed(
        &self,
        lock: &mut jsg::Lock<'_>,
        bytes: Vec<u8>,
        expiration: Option<f64>,
    ) -> jsg::Result<()> {
        let Some(Permit(permit)) = self.permit.take(lock)? else {
            return Ok(());
        };
        let result = IoContext::current(lock).and_then(|context| {
            let now = now_ms(&context)?;
            let trace = permit
                .succeed(bytes, expiration, now)
                .map_err(cache_error)?;
            trace::record_write(&context, &self.key, &trace)?;
            self.tag_fallback(
                lock,
                c"waiters_notified",
                trace::count(trace.waiters_notified),
            )
        });
        if let Err(err) = result {
            tracing::error!(error = %err, "memory cache fallback completion failed");
        }
        Ok(())
    }
}

/// A promise for the fields of what the fallback produced, as C++ JSG converts the value a
/// callback returns to a `jsg::Promise<CacheValueProduceResult>`: a promise is followed, and so
/// is a thenable if the `unwrap_custom_thenables` compatibility flag is on, through a reaction
/// that reads its result. Anything else is read now. The fields are held in a [`Produced`].
fn produce<'s>(
    lock: &mut jsg::Lock<'s>,
    produced: v8::Local<'s, v8::Value>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let promise = if let Ok(promise) = v8::Local::<v8::Promise>::try_from(produced) {
        promise
    } else if lock.feature_flags().get_unwrap_custom_thenables() && is_thenable(lock, produced)? {
        v8::Promise::resolved(lock, produced)?
    } else {
        let produced = Produced::read(lock, produced, Produce::Returned)?;
        return v8::Promise::resolved(lock, produced.0.into());
    };
    promise.then(lock, NoData, |lock, _: &NoData, result| {
        Ok(Produced::read(lock, result, Produce::Resolved)?.0.into())
    })
}

/// The fields of what a fallback produced, as a promise passes them on: in an object with a
/// `null` prototype, as C++ JSG holds a C++ value, so that resolving a promise with it follows
/// nothing, and reading its fields runs no JavaScript.
struct Produced<'s>(v8::Local<'s, v8::Object>);

impl<'s> Produced<'s> {
    fn read(
        lock: &mut jsg::Lock<'s>,
        result: v8::Local<'s, v8::Value>,
        produce: Produce,
    ) -> jsg::Result<Self> {
        let (value, expiration) = read_result(lock, result, produce)?;
        let fields = lock.new_object_with_null_prototype();
        fields.set(lock, "value", value)?;
        if let Some(expiration) = expiration {
            let expiration = v8::Number::new(lock, expiration);
            fields.set(lock, "expiration", expiration.into())?;
        }
        Ok(Self(fields))
    }

    fn fields(
        lock: &mut jsg::Lock<'s>,
        produced: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<(v8::Local<'s, v8::Value>, Option<f64>)> {
        let fields = v8::Local::<v8::Object>::try_from(produced).map_err(|_| {
            jsg::Error::new_internal_error("the memory cache fallback's fields are not an object")
        })?;
        let value = fields.get(lock, "value")?;
        let expiration = fields.get(lock, "expiration")?;
        let expiration = if expiration.is_undefined() {
            None
        } else {
            Some(f64::from_js(lock, expiration)?)
        };
        Ok((value, expiration))
    }
}

/// Caches what the fallback produced, and fulfills the read with its value, as the `.then()`
/// of memory-cache.c++ does. What fails rejects the promise, for [`rejected`] to handle. Tags the
/// fallback's span as memory-cache.c++ does, so that a failure leaves the same tags.
fn fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    fill: &SharedFill,
    produced: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    // Before serializing, which can run JavaScript, as memory-cache.c++ reaches the spans first.
    fill.0.require_current(lock)?;
    let (value, expiration) = Produced::fields(lock, produced)?;
    let bytes = match serialize(lock, value)? {
        Ok(bytes) => bytes,
        Err(exception) => return Err(lock.throw_value(exception)),
    };
    fill.0
        .tag_fallback(lock, c"fallback_result_size", trace::count(bytes.len()))?;
    if expiration.is_some_and(f64::is_nan) {
        return Err(jsg::Error::new_type_error(
            "Expiration time must not be NaN.",
        ));
    }
    fill.0
        .tag_fallback(lock, c"has_expiration", expiration.is_some())?;
    // Only now, so that a failure above does not overwrite it.
    fill.0.tag_fallback(lock, c"fallback_success", true)?;
    fill.0.succeed(lock, bytes, expiration)?;
    Ok(value)
}

/// Abandons the fallback, and rejects the read with what failed, as the `.catch_()` of
/// memory-cache.c++ does.
fn rejected<'a>(
    lock: &mut jsg::Lock<'a>,
    fill: &SharedFill,
    reason: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    fill.0.fail(lock, reason)?;
    Err(lock.throw_value(reason))
}

/// Serializes the value a fallback produced. Fails only if execution is terminating; otherwise
/// an exception is what the read rejects with.
fn serialize<'a>(
    lock: &mut jsg::Lock<'a>,
    value: v8::Local<'a, v8::Value>,
) -> jsg::Result<Result<Vec<u8>, v8::Local<'a, v8::Value>>> {
    let exception = match lock.try_catch(|lock| jsg::ser::serialize(lock, value))? {
        Ok(bytes) => return Ok(Ok(bytes)),
        Err(exception) => exception,
    };
    // What memory-cache.c++ rejects with, or what converting the exception threw.
    let rejection = lock.try_catch(|lock| serialization_error(lock, value, exception))?;
    Ok(Err(rejection.unwrap_or_else(|thrown| thrown)))
}

/// What memory-cache.c++ makes of what serializing `value` threw, which it throws on as a
/// `kj::Exception`: a `DOMException`, such as the `DataCloneError` that the serializer throws,
/// or an exception with an empty detail string becomes a `DataCloneError` that says only what
/// type failed, and any other exception becomes the copy that tunnelling it through the
/// `kj::Exception` makes.
fn serialization_error<'a>(
    lock: &mut jsg::Lock<'a>,
    value: v8::Local<'a, v8::Value>,
    exception: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    if jsg::try_unwrap_dom_exception(lock, exception)?.is_some()
        || exception.to_detail_string(lock)?.is_empty()
    {
        let err = jsg::Error::new_data_clone_error(format!(
            "failed to serialize {}",
            value.type_of(lock)
        ));
        return lock.exception(&err);
    }
    jsg::TunneledException::from_js(lock, exception)?.to_js(lock)
}

/// How a fallback produced a value.
#[derive(Clone, Copy)]
enum Produce {
    /// It returned it.
    Returned,
    /// The promise it returned resolved with it.
    Resolved,
}

/// Whether `value` is an object with a `then` property, as C++ JSG checks.
fn is_thenable<'a>(lock: &mut jsg::Lock<'a>, value: v8::Local<'a, v8::Value>) -> jsg::Result<bool> {
    match v8::Local::<v8::Object>::try_from(value) {
        Ok(object) => object.has(lock, "then"),
        Err(_) => Ok(false),
    }
}

/// The `value` and `expiration` of what a fallback produced, read as C++ JSG reads a
/// `CacheValueProduceResult`, and failing with the same messages. Those name the type as
/// `jsg::typeName()` does, rather than mangled, as the C++ for an undefined or null value does.
fn read_result<'a>(
    lock: &mut jsg::Lock<'a>,
    result: v8::Local<'a, v8::Value>,
    produce: Produce,
) -> jsg::Result<(v8::Local<'a, v8::Value>, Option<f64>)> {
    if result.is_null_or_undefined() {
        return Err(jsg::Error::new_type_error(
            "Cannot initialize CacheValueProduceResult with required members from an undefined \
             or null value.",
        ));
    }
    let result = v8::Local::<v8::Object>::try_from(result).map_err(|_| {
        jsg::Error::new_type_error(match produce {
            Produce::Returned => "Callback returned incorrect type; expected 'Promise'",
            Produce::Resolved => {
                "Incorrect type for Promise: the Promise did not resolve to \
                 'CacheValueProduceResult'."
            }
        })
    })?;
    let value = result.get(lock, "value")?;
    let expiration = result.get(lock, "expiration")?;
    let expiration = if expiration.is_undefined() {
        None
    } else {
        Some(f64::from_js(lock, expiration)?)
    };
    Ok((value, expiration))
}
