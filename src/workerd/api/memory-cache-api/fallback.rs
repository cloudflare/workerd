// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A read that waits for the cache: as the leader, until it runs its fallback, or as a waiter,
//! until the read it waits for finishes.

use std::cell::RefCell;

use io_context::IoContext;
use io_context::IoOwned;
use io_context::SpanBuilder;
use jsg::FromJS;
use jsg::v8;
use memory_cache::CacheError;
use memory_cache::FallbackPermit;

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
            Ok(Ok(value)) => {
                let _ = resolver.resolve(lock, value);
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
            Ok(fill) => fill,
            Err(err) => return reject(lock, resolver, &err),
        };
        let fallback = fallback.as_local(lock);
        // With `globalThis` as `this`, as C++ JSG calls a `jsg::Function`.
        let global = lock.global_object();
        match lock.try_catch(|lock| fallback.call(lock, Some(global.into()), &[key_js.into()])) {
            Ok(Ok(produced)) => fill.attach(lock, resolver, produced),
            // A fallback that throws rejects the read, as one whose promise rejects does.
            Ok(Err(exception)) => fill.reject_now(lock, resolver, exception),
            // Execution is terminating.
            Err(_) => {}
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
struct Fill {
    permit: IoOwned<FallbackPermit>,
    /// The key, for the span of the write.
    key: String,
    spans: RefCell<FillSpans>,
    /// What the fallback produced, if it returned it rather than a promise for it.
    produced: jsg::RefCell<Option<Produced>>,
}

/// The fields of what a fallback returned, read as it returned.
struct Produced {
    value: v8::TracedGlobal<v8::Value>,
    expiration: Option<f64>,
}

impl jsg::Traced for Produced {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.value.trace(visitor);
    }
}

/// The spans that end once the fallback settles, when the reactions to its promise drop the
/// [`Fill`] that holds them, as in memory-cache.c++.
struct FillSpans {
    _read: SpanBuilder,
    fallback: SpanBuilder,
}

impl jsg::Traced for Fill {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.produced.trace(visitor);
    }
}

impl Fill {
    /// Gives the request `permit`, so that it is dropped with the request: a fallback that never
    /// settles then does not keep the reads waiting for it until the garbage collector drops its
    /// reactions. Starts the span of the fallback.
    fn new(
        lock: &mut jsg::Lock<'_>,
        permit: FallbackPermit,
        key: &[u8],
        mut read_span: SpanBuilder,
    ) -> jsg::Result<Self> {
        let key = String::from_utf8_lossy(key).into_owned();
        let mut fallback_span = read_span.new_child(c"memory_cache_fallback");
        fallback_span.set_tag(c"key", &key);
        Ok(Self {
            permit: IoContext::current(lock)?.add_object(permit)?,
            key,
            spans: RefCell::new(FillSpans {
                _read: read_span,
                fallback: fallback_span,
            }),
            produced: jsg::RefCell::new(None),
        })
    }

    /// Settles the read as the promise of what the fallback produced settles.
    fn attach<'s>(
        self,
        lock: &mut jsg::Lock<'s>,
        resolver: v8::Local<'s, v8::PromiseResolver>,
        produced: v8::Local<'s, v8::Value>,
    ) {
        let promise = match lock.try_catch(|lock| self.promise_for(lock, produced)) {
            Ok(Ok(promise)) => promise,
            // As if the fallback threw it.
            Ok(Err(exception)) => return self.reject_now(lock, resolver, exception),
            // Execution is terminating.
            Err(_) => return,
        };
        match promise.then_or_catch(lock, self, fulfilled, rejected) {
            Ok(filled) => {
                let _ = resolver.resolve(lock, filled.into());
            }
            Err(err) => reject(lock, resolver, &err),
        }
    }

    /// A promise for what the fallback produced, as C++ JSG converts the value a callback
    /// returns to a `jsg::Promise<CacheValueProduceResult>`: a promise is taken as is, and a
    /// thenable is followed if the `unwrap_custom_thenables` compatibility flag is on. Anything
    /// else is read now, into [`Fill::produced`], and the promise resolves with `undefined`.
    fn promise_for<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        produced: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        if let Ok(promise) = v8::Local::<v8::Promise>::try_from(produced) {
            return Ok(promise);
        }
        if lock.feature_flags().get_unwrap_custom_thenables() && is_thenable(lock, produced)? {
            return v8::Promise::resolved(lock, produced);
        }
        let (value, expiration) = read_result(lock, produced, Produce::Returned)?;
        let value = v8::TracedGlobal::new(lock, value);
        *self.produced.borrow_mut(lock) = Some(Produced { value, expiration });
        let undefined = lock.undefined();
        v8::Promise::resolved(lock, undefined)
    }

    /// Fails as [`Fill::fail`] does, for a fallback that threw, and rejects the read with what
    /// it threw.
    fn reject_now<'s>(
        self,
        lock: &mut jsg::Lock<'s>,
        resolver: v8::Local<'s, v8::PromiseResolver>,
        exception: v8::Local<'s, v8::Value>,
    ) {
        match self.fail(lock, exception) {
            Ok(()) => {
                let _ = resolver.reject(lock, exception);
            }
            Err(err) => reject(lock, resolver, &err),
        }
    }

    fn tag_fallback<'v>(
        &self,
        key: &'static std::ffi::CStr,
        value: impl Into<io_context::TagValue<'v>>,
    ) {
        self.spans.borrow_mut().fallback.set_tag(key, value);
    }

    /// Records why the fallback failed, and abandons it, so that a read waiting for it runs its
    /// own. Fails if execution is terminating, or, as memory-cache.c++ does, if the request that
    /// ran the fallback is not the current one.
    fn fail<'a>(
        &self,
        lock: &mut jsg::Lock<'a>,
        exception: v8::Local<'a, v8::Value>,
    ) -> jsg::Result<()> {
        self.tag_fallback(c"fallback_success", false);
        self.tag_fallback(c"fallback_error", &exception.to_detail_string(lock)?);
        drop(self.permit.take(lock)?);
        Ok(())
    }

    /// Fails as [`Fill::fail`] does, and rejects with `exception`.
    fn reject<'a>(
        &self,
        lock: &mut jsg::Lock<'a>,
        exception: v8::Local<'a, v8::Value>,
    ) -> jsg::Result<v8::Local<'a, v8::Value>> {
        self.fail(lock, exception)?;
        Ok(v8::Promise::rejected(lock, exception)?.into())
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
        let Some(permit) = self.permit.take(lock)? else {
            return Ok(());
        };
        let result = IoContext::current(lock).and_then(|context| {
            let now = now_ms(&context)?;
            let trace = permit
                .succeed(bytes, expiration, now)
                .map_err(cache_error)?;
            trace::record_write(&context, &self.key, &trace)?;
            self.tag_fallback(c"waiters_notified", trace::count(trace.waiters_notified));
            Ok(())
        });
        if let Err(err) = result {
            tracing::error!(error = %err, "memory cache fallback completion failed");
        }
        Ok(())
    }
}

/// Caches what the fallback produced, and fulfills the read with its value. If the value cannot
/// be cached, abandons the fallback and rejects the read. Tags the fallback's span as
/// memory-cache.c++ does, so that a failure leaves the same tags.
fn fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    fill: &Fill,
    result: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    let produced = fill.produced.borrow_mut(lock).take();
    let (value, expiration) = match produced {
        Some(Produced { value, expiration }) => (value.as_local(lock), expiration),
        None => match lock.try_catch(|lock| read_result(lock, result, Produce::Resolved))? {
            Ok(fields) => fields,
            Err(exception) => return fill.reject(lock, exception),
        },
    };
    let bytes = match serialize(lock, value)? {
        Ok(bytes) => bytes,
        Err(exception) => return fill.reject(lock, exception),
    };
    fill.tag_fallback(c"fallback_result_size", trace::count(bytes.len()));
    if expiration.is_some_and(f64::is_nan) {
        let err = jsg::Error::new_type_error("Expiration time must not be NaN.");
        let exception = lock.exception(&err)?;
        return fill.reject(lock, exception);
    }
    fill.tag_fallback(c"has_expiration", expiration.is_some());
    // Only now, so that a failure above does not overwrite it.
    fill.tag_fallback(c"fallback_success", true);
    fill.succeed(lock, bytes, expiration)?;
    Ok(value)
}

/// Abandons the fallback, and rejects the read with what the fallback's promise rejected with.
fn rejected<'a>(
    lock: &mut jsg::Lock<'a>,
    fill: &Fill,
    reason: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    fill.reject(lock, reason)
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
/// `kj::Exception`: an exception with an empty detail string, such as the `DataCloneError` that
/// the serializer throws, becomes a `DataCloneError` that says only what type failed, and any
/// other exception becomes the copy that tunnelling it through the `kj::Exception` makes.
fn serialization_error<'a>(
    lock: &mut jsg::Lock<'a>,
    value: v8::Local<'a, v8::Value>,
    exception: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    if exception.to_detail_string(lock)?.is_empty() {
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
