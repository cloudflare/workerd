#![forbid(unsafe_code)]

//! Thread-safe storage and singleflight coordination for the memory cache.
//!
//! The C++ API owns a [`Namespace`] for each provider and creates a [`Binding`] for every configured
//! cache. Named bindings share a [`Cache`]; private bindings receive a dedicated one. Each binding
//! contributes limits, and the cache uses the component-wise maximum of all live bindings after
//! applying the namespace's policy cap.
//!
//! ```text
//!                         Namespace
//!                    (names + policy cap)
//!                            |
//!              bind(name, limits) / private binding
//!                            |
//!                            v
//!            +--------------------------------+
//!            | Cache: one logical cache       |
//!            |                                |
//!            | Mutex<CacheState>              |
//!            |  - LRU entries + expirations   |
//!            |  - live binding limits         |
//!            |  - weak in-flight fallbacks ---+----+
//!            +--------------------------------+    |
//!                            ^                     v
//!                         Binding         InFlightFallback(key)
//!                                            /                 \
//!                                  FallbackPermit             Waiters
//!                                     (leader)          /          \
//!                                        |       published value   gate acquired
//!                                        |              |               |
//!                                        +-- succeed -->+        next leader
//! ```
//!
//! A cache miss with fallback elects one caller as the leader. Its [`FallbackPermit`] holds the
//! fallback gate while C++ computes the value without holding the cache mutex. Other callers become
//! [`Waiter`] futures. A successful leader publishes one shared [`Bytes`] allocation to the cache
//! and all waiters. If the leader abandons its permit, the gate opens and exactly one waiter becomes
//! the next leader. Weak namespace and in-flight fallback indexes avoid keeping otherwise-unused
//! caches or abandoned work alive.

use std::cmp::Ordering;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::hash_map::RandomState;
use std::error::Error;
use std::fmt;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;
use std::sync::Weak;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering as AtomicOrdering;
use std::task::Context;
use std::task::Poll;
use std::time::Instant;

use bytes::Bytes;
use futures::future::Either;
use futures::future::select;
use hashlink::LinkedHashMap;
use tokio::sync::Mutex as AsyncMutex;
use tokio::sync::OwnedMutexGuard;
use tokio::sync::watch;

type OrderedMap<K, V> = LinkedHashMap<K, V, RandomState>;
type SharedValue = Bytes;

/// Identifies which payload a synchronous read decision contains.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadKind {
    Miss,
    Value,
    Leader,
    Waiter,
}

/// Controls whether a miss is returned immediately or joins singleflight fallback coordination.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadMode {
    CacheOnly,
    WithFallback,
}

/// Identifies whether a completed wait produced a value or promoted the waiter to leader.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitKind {
    Value,
    Leader,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EvictionReason {
    Expiration,
    Lru,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteOutcome {
    Success {
        is_update: bool,
        total_after: usize,
        entries_after: usize,
    },
    ValueTooLarge {
        max_value_size: usize,
    },
    AlreadyExpired,
}

/// The configured capacity requested by one binding or computed for a cache cache.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Limits {
    pub max_keys: u32,
    pub max_value_size: u32,
    pub max_total_value_size: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CacheError {
    IdExhausted(&'static str),
    CounterOverflow(&'static str),
    InvalidDecision(&'static str),
}

impl fmt::Display for CacheError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IdExhausted(kind) => write!(formatter, "memory cache {kind} IDs exhausted"),
            Self::CounterOverflow(kind) => {
                write!(formatter, "memory cache {kind} counter overflow")
            }
            Self::InvalidDecision(message) => formatter.write_str(message),
        }
    }
}

impl Error for CacheError {}

#[derive(Clone, Copy)]
pub struct ReadTrace {
    pub cache_hit: bool,
    pub entry_size: usize,
    pub total_value_size: usize,
    pub entry_count: usize,
    pub waiters_ahead: usize,
    pub lock_wait_ns: u64,
}

pub struct EvictionTrace {
    pub reason: EvictionReason,
    pub key: Arc<[u8]>,
    pub value_size: usize,
    pub total_before: usize,
    pub entries_before: usize,
}

pub struct WriteTrace {
    pub value_size: usize,
    pub has_expiration: bool,
    pub outcome: WriteOutcome,
    pub evictions: Vec<EvictionTrace>,
    pub waiters_notified: usize,
}

pub struct Stats {
    pub limits: Limits,
    pub bindings: usize,
    pub entries: usize,
    pub in_flight_fallbacks: usize,
    pub waiters: usize,
    pub canceled_waiters: usize,
}

impl Limits {
    fn normalize(mut self) -> Self {
        if self.max_keys == 0 || self.max_value_size == 0 || self.max_total_value_size == 0 {
            return Self::default();
        }
        self.max_value_size = self
            .max_value_size
            .min(self.max_total_value_size.try_into().unwrap_or(u32::MAX));
        self
    }

    fn component_max(self, other: Self) -> Self {
        Self {
            max_keys: self.max_keys.max(other.max_keys),
            max_value_size: self.max_value_size.max(other.max_value_size),
            max_total_value_size: self.max_total_value_size.max(other.max_total_value_size),
        }
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn elapsed_ns(start: Instant) -> u64 {
    start.elapsed().as_nanos().try_into().unwrap_or(u64::MAX)
}

fn saturating_increment(counter: &AtomicUsize) -> usize {
    counter
        .try_update(AtomicOrdering::Relaxed, AtomicOrdering::Relaxed, |value| {
            Some(value.saturating_add(1))
        })
        .unwrap_or(usize::MAX)
}

/// Defines the sharing boundary for named caches and applies provider-wide policy.
///
/// Its name index contains weak references so the namespace does not extend a cache's lifetime.
pub struct Namespace {
    inner: Arc<NamespaceInner>,
}

struct NamespaceInner {
    state: Mutex<NamespaceState>,
    max_total_value_size: Option<u64>,
}

#[derive(Default)]
struct NamespaceState {
    named: HashMap<Arc<str>, Weak<Cache>>,
}

/// One private or named logical cache.
///
/// All entry, binding, and fallback-index mutations are serialized by `state`. Waiter counters
/// remain atomic because cancellation and polling do not need to acquire that mutex merely for
/// telemetry.
struct Cache {
    namespace: Weak<NamespaceInner>,
    name: Option<Arc<str>>,
    state: Mutex<CacheState>,
    live_waiters: AtomicUsize,
    canceled_waiters: AtomicUsize,
}

/// Mutable state protected by a cache's synchronous mutex.
///
/// `entries` supplies LRU order, while `expirations` supplies expiration order. The fallback map is
/// a weak index into asynchronous work whose lifetime is owned by leaders and waiters.
#[derive(Default)]
struct CacheState {
    entries: OrderedMap<Arc<[u8]>, Entry>,
    expirations: BTreeSet<ExpirationRecord>,
    total_value_size: usize,
    bindings: HashMap<u64, Limits>,
    next_binding_id: u64,
    effective_limits: Limits,
    in_flight_fallbacks: HashMap<Arc<[u8]>, Weak<InFlightFallback>>,
}

/// A cached value and its optional absolute expiration time.
struct Entry {
    value: SharedValue,
    expiration: Option<f64>,
}

/// Secondary index record used to find expired entries before falling back to LRU eviction.
#[derive(Clone)]
struct ExpirationRecord {
    expiration: f64,
    key: Arc<[u8]>,
}

impl PartialEq for ExpirationRecord {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}

impl Eq for ExpirationRecord {}

impl Ord for ExpirationRecord {
    fn cmp(&self, other: &Self) -> Ordering {
        self.expiration
            .total_cmp(&other.expiration)
            .then_with(|| self.key.cmp(&other.key))
    }
}

impl PartialOrd for ExpirationRecord {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// Coordinates one in-progress fallback for one key.
///
/// The async mutex elects leaders, while the watch channel broadcasts a successful value without
/// requiring each waiter to reacquire the cache-state mutex.
struct InFlightFallback {
    cache: Arc<Cache>,
    key: Arc<[u8]>,
    gate: Arc<AsyncMutex<()>>,
    completion: watch::Sender<Option<SharedValue>>,
    waiters: AtomicUsize,
}

enum WaitResolution {
    Value(SharedValue),
    Leader(OwnedMutexGuard<()>),
}

#[derive(Clone, Copy)]
enum WaiterExit {
    Completed,
    Canceled,
}

/// One configured consumer's access to a cache and contribution to its effective limits.
///
/// Releasing the binding removes its limits and immediately resizes the cache if the effective
/// limits shrink.
pub struct Binding {
    namespace: Arc<NamespaceInner>,
    cache: Arc<Cache>,
    id: Option<u64>,
}

/// An immutable shared cached value returned across the FFI boundary without another byte copy.
pub struct Value {
    bytes: SharedValue,
}

/// The immediate result of a read, pairing telemetry with exactly one outcome payload.
pub struct ReadDecision {
    trace: ReadTrace,
    outcome: ReadOutcome,
}

/// The result of awaiting a coalesced fallback: either its value or leadership of the same operation.
pub struct WaitOutcome {
    outcome: WaitResult,
}

enum ReadOutcome {
    Miss,
    Value(Option<Value>),
    Leader(Option<FallbackPermit>),
    Waiter(Option<Waiter>),
}

enum WaitResult {
    Value(Option<Value>),
    Leader(Option<FallbackPermit>),
}

/// Exclusive permission to complete an in-flight fallback.
///
/// Dropping the permit without succeeding releases the gate so a waiter can take over.
pub struct FallbackPermit {
    // Fallback ownership drops before the guard can unlock an abandoned operation.
    in_flight: Arc<InFlightFallback>,
    _guard: OwnedMutexGuard<()>,
}

/// A cancellation-safe future waiting for either the published value or the fallback gate.
///
/// Dropping it unregisters its waiter counters and removes its lock attempt from the async mutex.
pub struct Waiter {
    // Fallback ownership drops before a canceled lock future leaves the mutex queue.
    in_flight: Option<Arc<InFlightFallback>>,
    wait: Option<Pin<Box<dyn Future<Output = WaitResolution> + Send>>>,
}

fn allocate_id(
    next: &mut u64,
    kind: &'static str,
    mut occupied: impl FnMut(u64) -> bool,
) -> Result<u64, CacheError> {
    let start = *next;
    loop {
        let candidate = next.checked_add(1).unwrap_or(1);
        *next = candidate;
        if !occupied(candidate) {
            return Ok(candidate);
        }
        if candidate == start {
            return Err(CacheError::IdExhausted(kind));
        }
    }
}

impl NamespaceInner {
    fn apply_policy(&self, mut limits: Limits) -> Limits {
        if let Some(cap) = self.max_total_value_size {
            limits.max_total_value_size = limits.max_total_value_size.min(cap);
        }
        limits.normalize()
    }
}

impl Drop for Cache {
    fn drop(&mut self) {
        let Some(name) = &self.name else {
            return;
        };
        let Some(namespace) = self.namespace.upgrade() else {
            return;
        };
        let mut state = lock(&namespace.state);
        if state
            .named
            .get(name)
            .is_some_and(|cache| std::ptr::eq(cache.as_ptr(), self))
        {
            state.named.remove(name);
        }
    }
}

impl Drop for InFlightFallback {
    fn drop(&mut self) {
        let mut state = lock(&self.cache.state);
        if state
            .in_flight_fallbacks
            .get(&self.key)
            .is_some_and(|current| std::ptr::eq(current.as_ptr(), self))
        {
            state.remove_in_flight_fallback(&self.key);
        }
    }
}

impl CacheState {
    fn allocate_binding_id(&mut self) -> Result<u64, CacheError> {
        allocate_id(&mut self.next_binding_id, "binding", |id| {
            self.bindings.contains_key(&id)
        })
    }

    fn is_exact_in_flight_fallback(&self, in_flight: &Arc<InFlightFallback>) -> bool {
        self.in_flight_fallbacks
            .get(&in_flight.key)
            .is_some_and(|current| std::ptr::eq(current.as_ptr(), Arc::as_ptr(in_flight)))
    }

    fn remove_in_flight_fallback(&mut self, key: &[u8]) -> Option<Weak<InFlightFallback>> {
        self.in_flight_fallbacks.remove(key)
    }

    fn remove_indexes(&mut self, key: &Arc<[u8]>, entry: &Entry) {
        if let Some(expiration) = entry.expiration {
            let removed = self.expirations.remove(&ExpirationRecord {
                expiration,
                key: Arc::clone(key),
            });
            debug_assert!(removed);
        }
    }

    fn insert_entry(&mut self, key: Arc<[u8]>, entry: Entry) -> Result<(), CacheError> {
        let total_value_size = self
            .total_value_size
            .checked_add(entry.value.len())
            .ok_or(CacheError::CounterOverflow("value size"))?;
        self.entries.reserve(1);
        if let Some(expiration) = entry.expiration {
            self.expirations.insert(ExpirationRecord {
                expiration,
                key: Arc::clone(&key),
            });
        }
        self.total_value_size = total_value_size;
        let replaced = self.entries.insert(key, entry);
        debug_assert!(replaced.is_none());
        Ok(())
    }

    fn remove_entry(&mut self, key: &[u8]) -> Option<(Arc<[u8]>, Entry)> {
        let (key, entry) = self.entries.remove_entry(key)?;
        self.remove_indexes(&key, &entry);
        self.total_value_size = self
            .total_value_size
            .checked_sub(entry.value.len())
            .unwrap_or_else(|| unreachable!("memory cache size accounting underflow"));
        Some((key, entry))
    }

    fn read_entry(&mut self, key: &[u8], now_ms: f64) -> Option<SharedValue> {
        if self.entries.get(key).is_some_and(|entry| {
            entry
                .expiration
                .is_some_and(|expiration| expiration < now_ms)
        }) {
            self.remove_entry(key);
            return None;
        }
        self.entries.to_back(key).map(|entry| entry.value.clone())
    }

    fn eviction_candidate(&self, now_ms: f64) -> Option<(Arc<[u8]>, EvictionReason)> {
        if let Some(expiration) = self.expirations.first()
            && expiration.expiration < now_ms
        {
            return Some((Arc::clone(&expiration.key), EvictionReason::Expiration));
        }
        self.entries
            .front()
            .map(|(key, _)| (Arc::clone(key), EvictionReason::Lru))
    }

    fn evict_one(&mut self, now_ms: f64, traces: Option<&mut Vec<EvictionTrace>>) -> bool {
        let Some((key, reason)) = self.eviction_candidate(now_ms) else {
            return false;
        };
        let total_before = self.total_value_size;
        let entries_before = self.entries.len();
        let Some((key, entry)) = self.remove_entry(&key) else {
            return false;
        };
        if let Some(traces) = traces {
            traces.push(EvictionTrace {
                reason,
                key,
                value_size: entry.value.len(),
                total_before,
                entries_before,
            });
        }
        true
    }

    fn resize(&mut self, now_ms: f64) {
        if self.effective_limits.max_keys == 0 {
            self.entries.clear();
            self.expirations.clear();
            self.total_value_size = 0;
            return;
        }
        let oversized: Vec<_> = self
            .entries
            .iter()
            .filter(|(_, entry)| entry.value.len() > self.effective_limits.max_value_size as usize)
            .map(|(key, _)| Arc::clone(key))
            .collect();
        for key in oversized {
            self.remove_entry(&key);
        }
        while self.total_value_size > self.effective_limits.max_total_value_size as usize
            || self.entries.len() > self.effective_limits.max_keys as usize
        {
            if !self.evict_one(now_ms, None) {
                break;
            }
        }
    }
}

impl Future for Waiter {
    type Output = WaitOutcome;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let result = match self
            .wait
            .as_mut()
            .unwrap_or_else(|| unreachable!("completed memory cache waiter was polled"))
            .as_mut()
            .poll(context)
        {
            Poll::Ready(result) => result,
            Poll::Pending => return Poll::Pending,
        };
        self.unregister(WaiterExit::Completed);
        self.wait = None;
        let in_flight = self
            .in_flight
            .take()
            .unwrap_or_else(|| unreachable!("completed memory cache waiter lost its fallback"));
        Poll::Ready(WaitOutcome {
            outcome: match result {
                WaitResolution::Value(bytes) => WaitResult::Value(Some(Value { bytes })),
                WaitResolution::Leader(guard) => WaitResult::Leader(Some(FallbackPermit {
                    in_flight,
                    _guard: guard,
                })),
            },
        })
    }
}

impl Waiter {
    fn unregister(&self, exit: WaiterExit) {
        let in_flight = self
            .in_flight
            .as_ref()
            .unwrap_or_else(|| unreachable!("completed memory cache waiter was still registered"));
        let previous = in_flight.waiters.fetch_sub(1, AtomicOrdering::Relaxed);
        debug_assert!(previous > 0);
        let previous = in_flight
            .cache
            .live_waiters
            .fetch_sub(1, AtomicOrdering::Relaxed);
        debug_assert!(previous > 0);
        if matches!(exit, WaiterExit::Canceled) {
            saturating_increment(&in_flight.cache.canceled_waiters);
        }
    }

    pub async fn wait(self) -> WaitOutcome {
        self.await
    }
}

impl Drop for Waiter {
    fn drop(&mut self) {
        if self.wait.is_some() {
            self.unregister(WaiterExit::Canceled);
        }
    }
}

async fn wait_for_in_flight_fallback(
    gate: Arc<AsyncMutex<()>>,
    mut completion: watch::Receiver<Option<SharedValue>>,
) -> WaitResolution {
    let published = completion.borrow().clone();
    if let Some(value) = published {
        return WaitResolution::Value(value);
    }

    let selected = {
        let lock = gate.lock_owned();
        let completed = wait_for_completion(&mut completion);
        futures::pin_mut!(lock, completed);
        match select(lock, completed).await {
            Either::Left((guard, _)) => WaitResolution::Leader(guard),
            Either::Right((value, _)) => WaitResolution::Value(value),
        }
    };
    match selected {
        WaitResolution::Leader(guard) => completion
            .borrow()
            .clone()
            .map_or_else(|| WaitResolution::Leader(guard), WaitResolution::Value),
        value @ WaitResolution::Value(_) => value,
    }
}

async fn wait_for_completion(completion: &mut watch::Receiver<Option<SharedValue>>) -> SharedValue {
    completion
        .wait_for(Option::is_some)
        .await
        .unwrap_or_else(|_| unreachable!("live memory cache fallback stopped publishing"));
    completion
        .borrow()
        .clone()
        .unwrap_or_else(|| unreachable!("memory cache fallback completion had no value"))
}

impl Binding {
    pub fn release(&mut self, now_ms: f64) {
        let Some(id) = self.id.take() else {
            return;
        };
        let mut state = lock(&self.cache.state);
        let removed = state.bindings.remove(&id);
        debug_assert!(removed.is_some());
        if recompute_limits(&mut state, &self.namespace) {
            state.resize(now_ms);
        }
    }
}

impl Drop for Binding {
    fn drop(&mut self) {
        // C++ normally releases explicitly with its process clock. This fallback is only for
        // exceptional bridge teardown; negative infinity preserves LRU behavior without guessing a clock.
        self.release(f64::NEG_INFINITY);
    }
}

fn recompute_limits(state: &mut CacheState, namespace: &NamespaceInner) -> bool {
    let limits = state
        .bindings
        .values()
        .copied()
        .fold(Limits::default(), Limits::component_max);
    let effective_limits = namespace.apply_policy(limits);
    if state.effective_limits == effective_limits {
        false
    } else {
        state.effective_limits = effective_limits;
        true
    }
}

fn new_cache(namespace: &Arc<NamespaceInner>, name: Option<Arc<str>>) -> Arc<Cache> {
    Arc::new(Cache {
        namespace: Arc::downgrade(namespace),
        name,
        state: Mutex::new(CacheState::default()),
        live_waiters: AtomicUsize::new(0),
        canceled_waiters: AtomicUsize::new(0),
    })
}

impl Namespace {
    pub fn new(max_total_value_size: Option<u64>) -> Self {
        Self {
            inner: Arc::new(NamespaceInner {
                state: Mutex::new(NamespaceState::default()),
                max_total_value_size,
            }),
        }
    }

    pub fn bind(&self, id: Option<&str>, limits: Limits) -> Result<Binding, CacheError> {
        let cache = if let Some(id) = id {
            let mut namespace_state = lock(&self.inner.state);
            let cache = if let Some(cache) = namespace_state.named.get(id).and_then(Weak::upgrade) {
                cache
            } else {
                let name: Arc<str> = Arc::from(id);
                let cache = new_cache(&self.inner, Some(Arc::clone(&name)));
                namespace_state.named.reserve(1);
                namespace_state.named.insert(name, Arc::downgrade(&cache));
                cache
            };
            drop(namespace_state);
            cache
        } else {
            new_cache(&self.inner, None)
        };
        let mut state = lock(&cache.state);
        let binding_id = state.allocate_binding_id()?;
        state.bindings.reserve(1);
        let limits = limits.normalize();
        state.bindings.insert(binding_id, limits);
        state.effective_limits = self
            .inner
            .apply_policy(state.effective_limits.component_max(limits));
        drop(state);
        Ok(Binding {
            namespace: Arc::clone(&self.inner),
            cache,
            id: Some(binding_id),
        })
    }
}

impl Binding {
    pub fn read(
        &self,
        key: &[u8],
        now_ms: f64,
        mode: ReadMode,
    ) -> Result<ReadDecision, CacheError> {
        let lock_start = Instant::now();
        let mut state = lock(&self.cache.state);
        let lock_wait_ns = elapsed_ns(lock_start);
        let value = state.read_entry(key, now_ms);
        let mut trace = ReadTrace {
            cache_hit: value.is_some(),
            entry_size: value.as_ref().map_or(0, Bytes::len),
            total_value_size: state.total_value_size,
            entry_count: state.entries.len(),
            waiters_ahead: 0,
            lock_wait_ns,
        };
        if let Some(bytes) = value {
            return Ok(ReadDecision {
                trace,
                outcome: ReadOutcome::Value(Some(Value { bytes })),
            });
        }
        if mode == ReadMode::CacheOnly {
            return Ok(ReadDecision {
                trace,
                outcome: ReadOutcome::Miss,
            });
        }
        if let Some(in_flight) = state.in_flight_fallbacks.get(key).and_then(Weak::upgrade) {
            trace.waiters_ahead = saturating_increment(&in_flight.waiters);
            saturating_increment(&self.cache.live_waiters);
            let wait = Box::pin(wait_for_in_flight_fallback(
                Arc::clone(&in_flight.gate),
                in_flight.completion.subscribe(),
            ));
            return Ok(ReadDecision {
                trace,
                outcome: ReadOutcome::Waiter(Some(Waiter {
                    in_flight: Some(in_flight),
                    wait: Some(wait),
                })),
            });
        }

        let gate = Arc::new(AsyncMutex::new(()));
        let (completion, _) = watch::channel(None);
        let guard = Arc::clone(&gate)
            .try_lock_owned()
            .unwrap_or_else(|_| unreachable!("new memory cache fallback gate was already locked"));
        let in_flight = Arc::new(InFlightFallback {
            cache: Arc::clone(&self.cache),
            key: Arc::from(key),
            gate,
            completion,
            waiters: AtomicUsize::new(0),
        });
        let permit = FallbackPermit {
            in_flight: Arc::clone(&in_flight),
            _guard: guard,
        };
        state.in_flight_fallbacks.reserve(1);
        state
            .in_flight_fallbacks
            .insert(Arc::clone(&in_flight.key), Arc::downgrade(&in_flight));
        drop(state);
        Ok(ReadDecision {
            trace,
            outcome: ReadOutcome::Leader(Some(permit)),
        })
    }

    pub fn delete(&self, key: &[u8]) {
        lock(&self.cache.state).remove_entry(key);
    }

    pub fn stats(&self) -> Stats {
        let state = lock(&self.cache.state);
        Stats {
            limits: state.effective_limits,
            bindings: state.bindings.len(),
            entries: state.entries.len(),
            in_flight_fallbacks: state
                .in_flight_fallbacks
                .values()
                .filter(|in_flight| in_flight.strong_count() > 0)
                .count(),
            waiters: self.cache.live_waiters.load(AtomicOrdering::Relaxed),
            canceled_waiters: self.cache.canceled_waiters.load(AtomicOrdering::Relaxed),
        }
    }
}

impl ReadDecision {
    pub fn kind(&self) -> ReadKind {
        match self.outcome {
            ReadOutcome::Miss => ReadKind::Miss,
            ReadOutcome::Value(_) => ReadKind::Value,
            ReadOutcome::Leader(_) => ReadKind::Leader,
            ReadOutcome::Waiter(_) => ReadKind::Waiter,
        }
    }

    pub fn trace(&self) -> ReadTrace {
        self.trace
    }

    pub fn take_value(&mut self) -> Result<Value, CacheError> {
        match &mut self.outcome {
            ReadOutcome::Value(value) => value.take().ok_or(CacheError::InvalidDecision(
                "read decision value was already taken",
            )),
            _ => Err(CacheError::InvalidDecision(
                "read decision does not contain a value",
            )),
        }
    }

    pub fn take_permit(&mut self) -> Result<FallbackPermit, CacheError> {
        match &mut self.outcome {
            ReadOutcome::Leader(permit) => permit.take().ok_or(CacheError::InvalidDecision(
                "read decision fallback permit was already taken",
            )),
            _ => Err(CacheError::InvalidDecision(
                "read decision does not contain a fallback permit",
            )),
        }
    }

    pub fn take_waiter(&mut self) -> Result<Waiter, CacheError> {
        match &mut self.outcome {
            ReadOutcome::Waiter(waiter) => waiter.take().ok_or(CacheError::InvalidDecision(
                "read decision waiter was already taken",
            )),
            _ => Err(CacheError::InvalidDecision(
                "read decision does not contain a waiter",
            )),
        }
    }
}

impl WaitOutcome {
    pub fn kind(&self) -> WaitKind {
        match self.outcome {
            WaitResult::Value(_) => WaitKind::Value,
            WaitResult::Leader(_) => WaitKind::Leader,
        }
    }

    pub fn take_value(&mut self) -> Result<Value, CacheError> {
        match &mut self.outcome {
            WaitResult::Value(value) => value.take().ok_or(CacheError::InvalidDecision(
                "wait outcome value was already taken",
            )),
            WaitResult::Leader(_) => Err(CacheError::InvalidDecision(
                "wait outcome does not contain a value",
            )),
        }
    }

    pub fn take_permit(&mut self) -> Result<FallbackPermit, CacheError> {
        match &mut self.outcome {
            WaitResult::Leader(permit) => permit.take().ok_or(CacheError::InvalidDecision(
                "wait outcome fallback permit was already taken",
            )),
            WaitResult::Value(_) => Err(CacheError::InvalidDecision(
                "wait outcome does not contain a fallback permit",
            )),
        }
    }
}

impl FallbackPermit {
    pub fn succeed(
        self,
        bytes: Vec<u8>,
        expiration: Option<f64>,
        now_ms: f64,
    ) -> Result<WriteTrace, CacheError> {
        let value = Bytes::from(bytes);
        let is_update;
        let mut trace = WriteTrace {
            value_size: value.len(),
            has_expiration: expiration.is_some(),
            outcome: WriteOutcome::Success {
                is_update: false,
                total_after: 0,
                entries_after: 0,
            },
            evictions: Vec::new(),
            waiters_notified: 0,
        };
        {
            let mut state = lock(&self.in_flight.cache.state);
            if !state.is_exact_in_flight_fallback(&self.in_flight) {
                return Ok(trace);
            }
            is_update = state.entries.contains_key(&self.in_flight.key);
            if state.effective_limits.max_keys == 0
                || state.effective_limits.max_total_value_size == 0
                || value.len() > state.effective_limits.max_value_size as usize
            {
                trace.outcome = WriteOutcome::ValueTooLarge {
                    max_value_size: state.effective_limits.max_value_size as usize,
                };
                state.remove_entry(&self.in_flight.key);
            } else if expiration.is_some_and(|expiration| expiration < now_ms) {
                trace.outcome = WriteOutcome::AlreadyExpired;
                state.remove_entry(&self.in_flight.key);
            } else {
                state.remove_entry(&self.in_flight.key);
                while state.entries.len() >= state.effective_limits.max_keys as usize
                    || state
                        .total_value_size
                        .checked_add(value.len())
                        .is_none_or(|size| {
                            size > state.effective_limits.max_total_value_size as usize
                        })
                {
                    if !state.evict_one(now_ms, Some(&mut trace.evictions)) {
                        break;
                    }
                }
                state.insert_entry(
                    Arc::clone(&self.in_flight.key),
                    Entry {
                        value: value.clone(),
                        expiration,
                    },
                )?;
                trace.outcome = WriteOutcome::Success {
                    is_update,
                    total_after: state.total_value_size,
                    entries_after: state.entries.len(),
                };
            }
            let previous = self.in_flight.completion.send_replace(Some(value));
            debug_assert!(previous.is_none());
            trace.waiters_notified = self.in_flight.waiters.load(AtomicOrdering::Relaxed);
            state.remove_in_flight_fallback(&self.in_flight.key);
        }
        Ok(trace)
    }
}

impl Value {
    pub fn bytes(&self) -> &[u8] {
        self.bytes.as_ref()
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
