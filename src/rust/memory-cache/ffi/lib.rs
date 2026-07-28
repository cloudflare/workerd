//! Generated workerd-cxx boundary for the safe memory-cache crate.

#![expect(
    clippy::needless_lifetimes,
    clippy::needless_pass_by_value,
    clippy::struct_field_names,
    clippy::unnecessary_box_returns,
    reason = "the CXX boundary requires these ownership and transport representations"
)]

#[cxx::bridge(namespace = "workerd::rust::memory_cache")]
mod ffi {
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum ReadKind {
        Miss,
        Value,
        Leader,
        Waiter,
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum ReadMode {
        CacheOnly,
        WithFallback,
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum WaitKind {
        Value,
        Leader,
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum EvictionReason {
        Expiration,
        Lru,
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum WriteOutcome {
        Success,
        ValueTooLarge,
        AlreadyExpired,
    }

    struct Limits {
        max_keys: u32,
        max_value_size: u32,
        max_total_value_size: u64,
    }

    struct ReadTrace {
        cache_hit: bool,
        entry_size: usize,
        total_value_size: usize,
        entry_count: usize,
        waiters_ahead: usize,
        lock_wait_ns: u64,
    }

    struct EvictionTrace {
        reason: EvictionReason,
        key: Vec<u8>,
        value_size: usize,
        total_before: usize,
        entries_before: usize,
    }

    struct WriteTrace {
        value_size: usize,
        has_expiration: bool,
        outcome: WriteOutcome,
        max_value_size: usize,
        is_update: bool,
        total_after: usize,
        entries_after: usize,
        evictions: Vec<EvictionTrace>,
        waiters_notified: usize,
    }

    struct Stats {
        bindings: usize,
        in_flight_fallbacks: usize,
        waiters: usize,
        canceled_waiters: usize,
    }

    extern "Rust" {
        type Namespace;
        type Binding;
        type ReadDecision;
        type Waiter;
        type WaitOutcome;
        type FallbackPermit;
        type Value;

        fn namespace_new(
            has_max_total_value_size: bool,
            max_total_value_size: u64,
        ) -> Box<Namespace>;
        fn bind(
            self: &Namespace,
            id: &str,
            is_private: bool,
            limits: Limits,
        ) -> Result<Box<Binding>>;
        fn release(self: &mut Binding, now_ms: f64);
        fn read(
            self: &Binding,
            key: &[u8],
            now_ms: f64,
            mode: ReadMode,
        ) -> Result<Box<ReadDecision>>;
        fn remove(self: &Binding, key: &[u8]);
        fn stats(self: &Binding) -> Stats;

        fn kind(self: &ReadDecision) -> ReadKind;
        fn trace(self: &ReadDecision) -> ReadTrace;
        fn take_value(self: &mut ReadDecision) -> Result<Box<Value>>;
        fn take_permit(self: &mut ReadDecision) -> Result<Box<FallbackPermit>>;
        fn take_waiter(self: &mut ReadDecision) -> Result<Box<Waiter>>;

        async fn waiter_wait(waiter: Box<Waiter>) -> Box<WaitOutcome>;
        fn kind(self: &WaitOutcome) -> WaitKind;
        fn take_value(self: &mut WaitOutcome) -> Result<Box<Value>>;
        fn take_permit(self: &mut WaitOutcome) -> Result<Box<FallbackPermit>>;

        fn succeed(
            self: &mut FallbackPermit,
            bytes: &[u8],
            has_expiration: bool,
            expiration: f64,
            now_ms: f64,
        ) -> Result<WriteTrace>;
        unsafe fn bytes<'a>(self: &'a Value) -> &'a [u8];
    }
}

struct Namespace {
    inner: memory_cache::Namespace,
}

struct Binding {
    inner: memory_cache::Binding,
}

struct ReadDecision {
    inner: memory_cache::ReadDecision,
}

struct Waiter {
    inner: memory_cache::Waiter,
}

struct WaitOutcome {
    inner: memory_cache::WaitOutcome,
}

struct FallbackPermit {
    inner: Option<memory_cache::FallbackPermit>,
}

struct Value {
    inner: memory_cache::Value,
}

fn limits_to_core(limits: ffi::Limits) -> memory_cache::Limits {
    memory_cache::Limits {
        max_keys: limits.max_keys,
        max_value_size: limits.max_value_size,
        max_total_value_size: limits.max_total_value_size,
    }
}

fn read_kind_from_core(kind: memory_cache::ReadKind) -> ffi::ReadKind {
    match kind {
        memory_cache::ReadKind::Miss => ffi::ReadKind::Miss,
        memory_cache::ReadKind::Value => ffi::ReadKind::Value,
        memory_cache::ReadKind::Leader => ffi::ReadKind::Leader,
        memory_cache::ReadKind::Waiter => ffi::ReadKind::Waiter,
    }
}

fn read_mode_to_core(
    mode: ffi::ReadMode,
) -> Result<memory_cache::ReadMode, memory_cache::CacheError> {
    match mode {
        ffi::ReadMode::CacheOnly => Ok(memory_cache::ReadMode::CacheOnly),
        ffi::ReadMode::WithFallback => Ok(memory_cache::ReadMode::WithFallback),
        _ => Err(memory_cache::CacheError::InvalidDecision(
            "unknown memory cache read mode",
        )),
    }
}

fn wait_kind_from_core(kind: memory_cache::WaitKind) -> ffi::WaitKind {
    match kind {
        memory_cache::WaitKind::Value => ffi::WaitKind::Value,
        memory_cache::WaitKind::Leader => ffi::WaitKind::Leader,
    }
}

fn eviction_reason_from_core(reason: memory_cache::EvictionReason) -> ffi::EvictionReason {
    match reason {
        memory_cache::EvictionReason::Expiration => ffi::EvictionReason::Expiration,
        memory_cache::EvictionReason::Lru => ffi::EvictionReason::Lru,
    }
}

fn namespace_new(has_max_total_value_size: bool, max_total_value_size: u64) -> Box<Namespace> {
    Box::new(Namespace {
        inner: memory_cache::Namespace::new(
            has_max_total_value_size.then_some(max_total_value_size),
        ),
    })
}

impl Namespace {
    fn bind(
        &self,
        id: &str,
        is_private: bool,
        limits: ffi::Limits,
    ) -> Result<Box<Binding>, memory_cache::CacheError> {
        Ok(Box::new(Binding {
            inner: self
                .inner
                .bind((!is_private).then_some(id), limits_to_core(limits))?,
        }))
    }
}

impl Binding {
    fn release(&mut self, now_ms: f64) {
        self.inner.release(now_ms);
    }

    fn read(
        &self,
        key: &[u8],
        now_ms: f64,
        mode: ffi::ReadMode,
    ) -> Result<Box<ReadDecision>, memory_cache::CacheError> {
        Ok(Box::new(ReadDecision {
            inner: self.inner.read(key, now_ms, read_mode_to_core(mode)?)?,
        }))
    }

    fn remove(&self, key: &[u8]) {
        self.inner.delete(key);
    }

    fn stats(&self) -> ffi::Stats {
        let stats = self.inner.stats();
        ffi::Stats {
            bindings: stats.bindings,
            in_flight_fallbacks: stats.in_flight_fallbacks,
            waiters: stats.waiters,
            canceled_waiters: stats.canceled_waiters,
        }
    }
}

impl ReadDecision {
    fn kind(&self) -> ffi::ReadKind {
        read_kind_from_core(self.inner.kind())
    }

    fn trace(&self) -> ffi::ReadTrace {
        let trace = self.inner.trace();
        ffi::ReadTrace {
            cache_hit: trace.cache_hit,
            entry_size: trace.entry_size,
            total_value_size: trace.total_value_size,
            entry_count: trace.entry_count,
            waiters_ahead: trace.waiters_ahead,
            lock_wait_ns: trace.lock_wait_ns,
        }
    }

    fn take_value(&mut self) -> Result<Box<Value>, memory_cache::CacheError> {
        Ok(Box::new(Value {
            inner: self.inner.take_value()?,
        }))
    }

    fn take_permit(&mut self) -> Result<Box<FallbackPermit>, memory_cache::CacheError> {
        Ok(Box::new(FallbackPermit {
            inner: Some(self.inner.take_permit()?),
        }))
    }

    fn take_waiter(&mut self) -> Result<Box<Waiter>, memory_cache::CacheError> {
        Ok(Box::new(Waiter {
            inner: self.inner.take_waiter()?,
        }))
    }
}

async fn waiter_wait(waiter: Box<Waiter>) -> Box<WaitOutcome> {
    Box::new(WaitOutcome {
        inner: waiter.inner.wait().await,
    })
}

impl WaitOutcome {
    fn kind(&self) -> ffi::WaitKind {
        wait_kind_from_core(self.inner.kind())
    }

    fn take_value(&mut self) -> Result<Box<Value>, memory_cache::CacheError> {
        Ok(Box::new(Value {
            inner: self.inner.take_value()?,
        }))
    }

    fn take_permit(&mut self) -> Result<Box<FallbackPermit>, memory_cache::CacheError> {
        Ok(Box::new(FallbackPermit {
            inner: Some(self.inner.take_permit()?),
        }))
    }
}

impl FallbackPermit {
    fn succeed(
        &mut self,
        bytes: &[u8],
        has_expiration: bool,
        expiration: f64,
        now_ms: f64,
    ) -> Result<ffi::WriteTrace, memory_cache::CacheError> {
        let trace = self
            .inner
            .take()
            .ok_or(memory_cache::CacheError::InvalidDecision(
                "fallback permit was already used",
            ))?
            .succeed(bytes.to_vec(), has_expiration.then_some(expiration), now_ms)?;
        let (outcome, max_value_size, is_update, total_after, entries_after) = match trace.outcome {
            memory_cache::WriteOutcome::Success {
                is_update,
                total_after,
                entries_after,
            } => (
                ffi::WriteOutcome::Success,
                0,
                is_update,
                total_after,
                entries_after,
            ),
            memory_cache::WriteOutcome::ValueTooLarge { max_value_size } => (
                ffi::WriteOutcome::ValueTooLarge,
                max_value_size,
                false,
                0,
                0,
            ),
            memory_cache::WriteOutcome::AlreadyExpired => {
                (ffi::WriteOutcome::AlreadyExpired, 0, false, 0, 0)
            }
        };
        Ok(ffi::WriteTrace {
            value_size: trace.value_size,
            has_expiration: trace.has_expiration,
            outcome,
            max_value_size,
            is_update,
            total_after,
            entries_after,
            evictions: trace
                .evictions
                .into_iter()
                .map(|eviction| ffi::EvictionTrace {
                    reason: eviction_reason_from_core(eviction.reason),
                    key: eviction.key.to_vec(),
                    value_size: eviction.value_size,
                    total_before: eviction.total_before,
                    entries_before: eviction.entries_before,
                })
                .collect(),
            waiters_notified: trace.waiters_notified,
        })
    }
}

impl Value {
    fn bytes(&self) -> &[u8] {
        self.inner.bytes()
    }
}
