// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `MemoryCache`, the JavaScript API of a memory cache binding, over the cache in
//! `src/rust/memory-cache`. It does what `src/workerd/api/memory-cache.c++` does, and records
//! the same trace spans.

mod fallback;
mod ffi;
mod trace;

use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use io_context::IoContext;
use jsg::Member;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use memory_cache::CacheError;
use memory_cache::ReadDecision;
use memory_cache::ReadKind;
use memory_cache::ReadMode;
use memory_cache::WaitKind;

use crate::fallback::PendingRead;
use crate::trace::ReadSpans;

/// The longest key, in UTF-8 bytes, as in memory-cache.c++.
const MAX_KEY_SIZE: usize = 2 * 1024;

#[jsg_resource(name = "MemoryCache")]
pub struct MemoryCache {
    binding: CacheBinding,
}

impl MemoryCache {
    fn new(binding: memory_cache::Binding) -> Self {
        Self {
            binding: CacheBinding(binding),
        }
    }
}

#[jsg_resource(custom_members)]
impl MemoryCache {
    /// `read(key, fallback?)`: a promise for the value cached under `key`. On a miss, without
    /// `fallback` it is `undefined`. With it, one read of all those that miss calls
    /// `fallback(key)`, caches what it produces and passes it to the others, which wait for it.
    /// If it fails, that read rejects with what it threw, and one of the others calls its own
    /// fallback.
    #[jsg_method]
    fn read<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        key: v8::Local<'s, v8::Value>,
        fallback: Option<v8::Local<'s, v8::Value>>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        // As a C++ method that returns a promise, it rejects rather than throws.
        match self.read_promise(lock, key, fallback) {
            Err(err) if !err.is_pending() => v8::Promise::rejected_with_error(lock, &err),
            result => result,
        }
    }

    /// `delete(key)`: removes the value cached under `key`, if any.
    #[jsg_method(name = "delete")]
    fn remove<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        key: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<()> {
        let key = key_bytes(lock, key)?;
        // Keys that are too large are never cached.
        if key.len() > MAX_KEY_SIZE {
            return Err(jsg::Error::new_range_error("Key too large."));
        }
        self.binding.0.delete(&key);
        Ok(())
    }
}

impl Resource for MemoryCache {
    fn members(flags: compatibility_flags::Reader<'_>) -> Vec<Member> {
        let mut members = vec![Self::read_member()];
        if flags.get_memory_cache_delete() {
            members.push(Self::remove_member());
        }
        members
    }
}

impl MemoryCache {
    fn read_promise<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        key: v8::Local<'s, v8::Value>,
        fallback: Option<v8::Local<'s, v8::Value>>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let key = key_bytes(lock, key)?;
        // JSG checks the parameters of C++'s read() before it runs, and starts any span.
        let fallback = fallback
            .map(|fallback| {
                v8::Local::<v8::Function>::try_from(fallback).map_err(|_| {
                    jsg::Error::new_type_error(
                        "Failed to execute 'read' on 'MemoryCache': parameter 2 is not of type \
                         'Function'.",
                    )
                })
            })
            .transpose()?;
        if key.len() > MAX_KEY_SIZE {
            return Err(jsg::Error::new_range_error("Key too large."));
        }
        let io_context = IoContext::current(lock)?;
        let mut spans = ReadSpans::start(&io_context)?;
        let now = now_ms(&io_context)?;

        let Some(fallback) = fallback else {
            let decision = self
                .binding
                .0
                .read(&key, now, ReadMode::CacheOnly)
                .map_err(cache_error)?;
            trace::tag_read(&mut spans.read, &decision.trace());
            return read_cached(lock, decision);
        };
        let decision = self
            .binding
            .0
            .read(&key, now, ReadMode::WithFallback)
            .map_err(cache_error)?;
        read_with_fallback(lock, &io_context, decision, fallback, key, spans)
    }
}

/// A read without a fallback: the cached value, or `undefined`.
fn read_cached<'s>(
    lock: &mut jsg::Lock<'s>,
    mut decision: ReadDecision,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let value = match decision.kind() {
        ReadKind::Value => {
            let value = decision.take_value().map_err(cache_error)?;
            jsg::ser::deserialize(lock, value.bytes())?
        }
        _ => lock.undefined(),
    };
    v8::Promise::resolved(lock, value)
}

/// A read with a fallback: the cached value, or the value the leader of the reads that miss
/// produces with its fallback.
fn read_with_fallback<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    mut decision: ReadDecision,
    fallback: v8::Local<'s, v8::Function>,
    key: Vec<u8>,
    mut spans: ReadSpans,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let read_trace = decision.trace();
    match decision.kind() {
        // Served without releasing the isolate lock.
        ReadKind::Value => {
            trace::tag_read(&mut spans.read, &read_trace);
            let value = decision.take_value().map_err(cache_error)?;
            let value = jsg::ser::deserialize(lock, value.bytes())?;
            v8::Promise::resolved(lock, value)
        }
        ReadKind::Leader => {
            trace::tag_leader(&mut spans.read, &read_trace);
            let permit = decision.take_permit().map_err(cache_error)?;
            let (pending, promise) = PendingRead::new(lock, fallback, key, spans)?;
            // As in C++, the fallback runs in a later turn, not while `read` is running.
            io_context.await_io(async move { permit }, move |lock, permit| {
                pending.run_fallback(lock, permit);
            })?;
            Ok(promise)
        }
        ReadKind::Waiter => {
            let wait_span =
                trace::tag_waiter(&mut spans.read, &read_trace, &String::from_utf8_lossy(&key));
            let waiter = decision.take_waiter().map_err(cache_error)?;
            let (pending, promise) = PendingRead::new(lock, fallback, key, spans)?;
            io_context.await_io(waiter.wait(), move |lock, mut outcome| {
                // The span of the wait ends with it. It cannot go in the future, which is `Send`,
                // so it ends when the callback runs, just after the wait completes.
                drop(wait_span);
                match outcome.kind() {
                    // The read it waited for cached a value, or failed to but produced one.
                    WaitKind::Value => pending.resolve_with(lock, outcome.take_value()),
                    // The read it waited for failed, and this one is next.
                    WaitKind::Leader => match outcome.take_permit() {
                        Ok(permit) => pending.run_fallback(lock, permit),
                        Err(err) => pending.reject(lock, &cache_error(err)),
                    },
                }
            })?;
            Ok(promise)
        }
        ReadKind::Miss => Err(jsg::Error::new_internal_error(
            "the memory cache missed without a fallback read",
        )),
    }
}

/// The cache binding. Its empty `Traced` impl is correct: the cache holds serialized bytes,
/// never JavaScript values, so the binding refers to nothing in the V8 heap.
struct CacheBinding(memory_cache::Binding);

impl jsg::Traced for CacheBinding {}

impl Drop for CacheBinding {
    fn drop(&mut self) {
        // memory-cache.c++ uses the request's clock if a request is current, and the system
        // clock otherwise. A drop has no lock to find the request with, so this always uses the
        // system clock. The clock matters only if releasing the binding shrinks the cache, to
        // tell which entries have expired, and a request's clock trails the system clock only
        // by the time since the request last waited for I/O.
        self.0.release(system_now_ms());
    }
}

/// `key` as UTF-8, with lone surrogates encoded as such, as C++ `kj::String` holds them, so that
/// keys differing only in them are distinct.
fn key_bytes<'s>(lock: &mut jsg::Lock<'s>, key: v8::Local<'s, v8::Value>) -> jsg::Result<Vec<u8>> {
    let key = v8::Local::<v8::String>::try_from(key).map_err(|_| {
        jsg::Error::new_type_error(
            "Failed to execute 'read' on 'MemoryCache': parameter 1 is not of type 'string'.",
        )
    })?;
    let mut bytes = vec![0; key.utf8_length(lock)];
    let written = key.write_utf8(lock, &mut bytes, v8::WriteFlags::None);
    bytes.truncate(written);
    Ok(bytes)
}

/// The request's clock in whole milliseconds since the Unix epoch, as C++ `dateNow()`.
fn now_ms(io_context: &IoContext<'_>) -> jsg::Result<f64> {
    Ok(millis_since_epoch(io_context.now()?))
}

/// The system clock, as memory-cache.c++ uses outside a request.
fn system_now_ms() -> f64 {
    millis_since_epoch(SystemTime::now())
}

#[expect(
    clippy::cast_precision_loss,
    reason = "milliseconds since the epoch fit in an f64 exactly"
)]
fn millis_since_epoch(time: SystemTime) -> f64 {
    time.duration_since(UNIX_EPOCH)
        .map_or(0.0, |since| since.as_millis() as f64)
}

/// A cache error, which only a bug causes, so JavaScript sees it as an internal error.
fn cache_error(err: CacheError) -> jsg::Error {
    jsg::Error::new_internal_error(format!("memory cache error: {err}"))
}
