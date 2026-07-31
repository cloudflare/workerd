'use strict';

// Single-queue / multi-cursor model backing the TypeScript Streams
// implementation: the QUEUED consumer backend. See the queue/cursor design
// doc for the full rationale, and native-stream-integration.md §10 for the
// fence conventions separating this backend from the native one.
//
// One StreamQueue per controller holds each chunk exactly once. Every
// consumer (the stream itself, and each tee branch) owns a QueueCursor — a
// logical position into the shared queue. Entries are reclaimed once every
// live cursor has advanced past them; the slowest cursor drives
// backpressure (desiredSize = highWaterMark - max(cursor remaining size)).
//
// QUEUED-BACKEND INVARIANTS (binding on changes to this file; the native
// backend in native.ts has a DIFFERENT set — do not port logic across the
// fence without checking both):
//   - The CLOSE_SENTINEL is always the LAST slot; it is the ONLY close-
//     propagation mechanism (drain-then-close per cursor; BYOB descriptors
//     stay pending at the sentinel per the spec footgun — the native
//     backend deliberately differs with its fused close-commit).
//   - read() takes the fast path ONLY when no reads are pending (per-reader
//     FIFO; entries and pending reads coexist under batched notification).
//   - Byte entries are {buffer, byteOffset, byteLength} triples; queue
//     internals never read view metadata through patchable getters.
//   - Copy-on-read for shared byte entries: copy iff any OTHER live cursor
//     still needs the entry (exact last-consumer test) — REQUIRED for
//     soundness, not just spec parity.
//   - Cursors hold weak owner refs; orphan pruning happens on every cursor
//     iteration, with the FinalizationRegistry as the idle-queue backstop
//     (the native backend needs NEITHER — JSG owns its source lifetime).
//   - desiredSize reflects the SLOWEST live cursor; a pending read on any
//     cursor overrides backpressure at the controller.
//
// Nothing in this module is ever exposed to user code: queues and cursors
// are held in #private fields of the stream classes. Method calls on these
// classes still follow the bootstrap primordials discipline (no bare
// prototype lookups on builtins, no for...of over Sets/arrays).

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  ReadableStreamReadResult,
} from './types';

const {
  ArrayBufferPrototypeSlice,
  ArrayPrototypePush,
  ArrayPrototypeShift,
  ArrayPrototypeSplice,
  FinalizationRegistry,
  FinalizationRegistryPrototypeRegister,
  FinalizationRegistryPrototypeUnregister,
  MathMin,
  ObjectCreate,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  ReflectConstruct,
  SafeSet,
  Symbol,
  TypeError,
  TypedArrayPrototypeSet,
  TypedArrayPrototypeSlice,
  Uint8Array,
  WeakRef,
  WeakRefPrototypeDeref,
} = primordials;

// Read-result objects are plain { value, done } objects with the default
// Object.prototype, matching spec behavior. Internal machinery is protected
// by captured primordials (PromisePrototypeThen, etc.), not by the result
// object's prototype chain. If the user patches Object.prototype.then, that
// only affects their own promise resolution — same trade-off as Node.js.
export function createReadResult<T>(
  value: T,
  done: false
): { value: T; done: false };
export function createReadResult<T>(
  value: T | undefined,
  done: true
): { value: T | undefined; done: true };
export function createReadResult<T>(
  value: T | undefined,
  done: boolean
): { value: T | undefined; done: boolean } {
  return { value, done };
}

// ---------------------------------------------------------------------------
// Entries and the close sentinel

export interface QueueEntry<T> {
  value: T;
  // As computed by the queuing strategy's size() (byteLength for byte
  // streams). Drives remainingSize/desiredSize accounting.
  size: number;
}

// End-of-stream marker stored in the entries array after close(). Always the
// LAST slot; nothing may be enqueued after it. The sentinel is the ONLY
// close-propagation mechanism: a cursor observes the close exactly when its
// position reaches the sentinel, guaranteeing it drains all buffered data
// first. Every consuming path must handle it explicitly (it has no value and
// no size).
const CLOSE_SENTINEL: symbol = Symbol('closeSentinel');

export type QueueSlot<T> = QueueEntry<T> | symbol;

function isQueueEntry<T>(
  slot: QueueSlot<T> | undefined
): slot is QueueEntry<T> {
  return slot !== undefined && slot !== CLOSE_SENTINEL;
}

// Byte queue entries are spec-shaped {buffer, byteOffset, byteLength}
// triples, NOT live ArrayBufferViews. The byte controller normalizes chunks
// into triples at the enqueue() trust boundary (after validation and buffer
// transfer), so queue internals never read view metadata through patchable
// %TypedArray%.prototype getters.
export interface ByteQueueEntry {
  buffer: ArrayBuffer;
  byteOffset: number;
  byteLength: number;
}

// ---------------------------------------------------------------------------
// Pending reads

export interface PendingRead<V> {
  resolve: (result: ReadableStreamReadResult<V>) => void;
  reject: (reason: unknown) => void;
  // The reader that submitted this read. Used for selective rejection when
  // a reader's lock is released (the cursor itself outlives readers).
  // Also keeps the reader (and through it, the owning stream) strongly
  // reachable, which is what guarantees a cursor with pending reads is
  // never orphaned.
  reader: object;
}

type ArrayBufferViewCtor = new (
  buffer: ArrayBuffer,
  byteOffset: number,
  length: number
) => ArrayBufferView;

// Spec-shaped pull-into descriptor ([[pendingPullIntos]] item). We never
// hold the caller's live view: its backing buffer is transferred at
// read(view) time (and re-transferred on every respond()), so the result
// view is reconstructed from these primitives only at fulfillment.
export interface PullIntoDescriptor {
  // CURRENT backing buffer — re-pointed by the controller after each
  // transfer (read(view), respond(), respondWithNewView()).
  buffer: ArrayBuffer;
  // Spec "buffer byte length": the buffer's byteLength at creation time.
  // Persists even when `buffer` has been transferred (detached → byteLength 0).
  // Used by respondWithNewView() to validate the replacement buffer matches.
  bufferByteLength: number;
  byteOffset: number; // start of the caller's view within buffer
  byteLength: number; // caller's view length in bytes
  bytesFilled: number; // accumulates across enqueues/responds
  minimumFill: number; // min × elementSize, in BYTES
  elementSize: number; // BYTES_PER_ELEMENT (1 for DataView)
  // Captured via safe view-type detection — NEVER view.constructor, which
  // is user-controllable via an own property.
  viewCtor: ArrayBufferViewCtor;
  // 'default' = synthetic descriptor for a default read on a byte stream
  // with autoAllocateChunkSize set.
  // 'none' = descriptor whose reader was released (releaseLock); respond()
  // enqueues the data rather than resolving a read promise.
  readerType: 'byob' | 'default' | 'none';
  promise: Promise<ReadableStreamReadResult<ArrayBufferView>>;
  resolve: (result: ReadableStreamReadResult<ArrayBufferView>) => void;
  reject: (reason: unknown) => void;
  reader: object;
}

// ---------------------------------------------------------------------------
// The consumer interface (the FENCE between backends)
//
// The reader layer in readable.ts programs against this contract and MUST
// remain backend-blind: it never knows whether it is talking to the queued
// cursor machinery in this file or the native pull conduit (native.ts).
// All backend divergence lives behind this interface or at the five
// enumerated BACKEND-DISPATCH points (see native-stream-integration.md
// §10 "Fences and signposts").

export interface StreamConsumer<V> {
  // Submit a default read. Per-reader FIFO; reader identity enables
  // selective rejection on lock release.
  read(reader: object): Promise<ReadableStreamReadResult<V>>;
  // Attempt a synchronous read. Returns the result directly when data (or
  // the close sentinel) is immediately available at the cursor, or
  // undefined when no data is buffered / reads are already queued (caller
  // must fall back to the async read() path). This avoids the microtask
  // gap that async/await introduces on an already-resolved promise,
  // preserving spec PullSteps timing for the drain-then-close check.
  tryReadSync(reader: object): ReadableStreamReadResult<V> | undefined;
  // Bulk read for the draining reader / pipeTo. Synchronous: collects what
  // is buffered (the draining reader handles the empty-then-wait case).
  drain(maxSize?: number): { chunks: V[]; done: boolean };
  // Reject pending reads submitted by a specific reader (lock release).
  cancelReadsForReader(reader: object, reason: unknown): void;
  // Reject all pending reads (stream ERROR path only).
  errorAllReads(reason: unknown): void;
  // Resolve all pending reads as done (stream CANCEL path).
  resolveAllReadsAsDone(): void;
  readonly hasPendingRead: boolean;
  // Fulfill the first pending read directly with a value, skipping the queue.
  // Spec ReadableStreamFulfillReadRequest: shift + chunk steps.
  fulfillFirstPendingRead(value: V): void;
  // Stream-cancel teardown. The STREAM layer owns the source-cancel policy
  // (tee composite hooks vs direct controller cancel) and passes it as
  // `decideSourceCancel`, invoked with whether this consumer is the last
  // one; the consumer owns the mechanics and the ordering guarantee (the
  // decision runs BEFORE consumer removal, so a reason-carrying cancel
  // wins any idempotency cache over GC-path hooks).
  cancelStream(
    reason: unknown,
    decideSourceCancel: (isLastConsumer: boolean) => Promise<void>
  ): Promise<void>;
}

// Byte-capable consumers additionally support the BYOB machinery. Both the
// ByteStreamCursor (queued) and the NativePullConduit (native) satisfy
// this.
export interface ByteStreamConsumer extends StreamConsumer<Uint8Array> {
  readBYOB(
    desc: PullIntoDescriptor
  ): Promise<ReadableStreamReadResult<ArrayBufferView>>;
  readonly hasPendingPullInto: boolean;
  readonly hasPartiallyFulfilledRead: boolean;
  readonly headPullInto: PullIntoDescriptor | undefined;
  readonly pendingPullIntoView: Uint8Array | undefined;
  respondBYOB(bytesWritten: number): void;
  commitPullIntosOnClose(): void;
  // Spec: enqueue() drains 'none'-typed head descriptors BEFORE adding
  // the new chunk. Called by the controller's enqueue() path.
  drainNoneDescriptors(): void;
  // Spec step 9.3: if the head descriptor is an auto-allocate
  // (readerType 'default'), shift it out and return it so the controller
  // can fulfill the read directly from the enqueued chunk.
  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined;
}

// ---------------------------------------------------------------------------
// Orphan-detection backstop
//
// Lazy WeakRef pruning (see #forEachLiveCursor) only runs inside queue
// operations. An idle queue whose consumers were all GC'd — but which is
// pinned from the C++ side via the controller — would otherwise never
// observe "all cursors gone" and never cancel the underlying source. The
// registry provides the active signal; cleanup scheduling is controlled by
// workerd and deliberately coarse.

interface CursorLike {
  queue: { removeCursor(cursor: unknown): void };
}

const cursorCleanupRegistry = new FinalizationRegistry((cursor: CursorLike) => {
  cursor.queue.removeCursor(cursor);
});

// ---------------------------------------------------------------------------
// StreamQueue

// T is the entry value type (ByteQueueEntry for byte streams); V is the type
// delivered to read results (Uint8Array remainder views for byte streams).
class StreamQueue<T, V = T> {
  #entries: QueueSlot<T>[] = [];
  #headOffset: number = 0; // logical index of #entries[0]
  #cursors: Set<QueueCursor<T, V>>;
  #highWaterMark: number;
  #state: 'readable' | 'closed' | 'errored' = 'readable';
  // Idempotent hook, wired to the controller: cancels the underlying source
  // when the last consumer goes away. "cursorCount === 0" is a state, not an
  // event — every path that can remove the last cursor funnels here.
  #onAllCursorsGone: () => void;
  #hadCursors: boolean = false;
  #allGoneSignaled: boolean = false;

  constructor(highWaterMark: number, onAllCursorsGone: () => void) {
    this.#cursors = new SafeSet();
    this.#highWaterMark = highWaterMark;
    this.#onAllCursorsGone = onAllCursorsGone;
  }

  // Iterate live cursors, pruning any whose owning stream has been GC'd.
  // This is the ONLY way cursor iteration happens — raw iteration would
  // skip orphan detection. SafeSet#forEach dispatches through the captured
  // SetPrototypeForEach (deleting during forEach is safe per spec; for...of
  // over a SafeSet is NOT pollution-safe and must not be used).
  #forEachLiveCursor(fn: (cursor: QueueCursor<T, V>) => void): void {
    this.#cursors.forEach((cursor) => {
      if (cursor.isOrphaned()) {
        this.#cursors.delete(cursor);
        unregisterCursorCleanup(cursor);
      } else {
        fn(cursor);
      }
    });
    this.#checkAllCursorsGone();
  }

  #checkAllCursorsGone(): void {
    if (
      this.#hadCursors &&
      !this.#allGoneSignaled &&
      this.#cursors.size === 0
    ) {
      this.#allGoneSignaled = true;
      this.#onAllCursorsGone();
    }
  }

  // The slowest cursor's backlog determines backpressure. Note that a
  // pending read on any cursor overrides backpressure at the controller
  // (shouldPull), so this is honest signaling, not a memory bound.
  get desiredSize(): number {
    let max = 0;
    this.#forEachLiveCursor((cursor) => {
      const remaining = cursor.remainingSize;
      if (remaining > max) max = remaining;
    });
    return this.#highWaterMark - max;
  }

  get length(): number {
    // Logical end position (one past the last slot).
    return this.#headOffset + this.#entries.length;
  }

  get cursorCount(): number {
    // Prune orphans, then report.
    this.#forEachLiveCursor(() => {});
    return this.#cursors.size;
  }

  // The single live cursor, if there is exactly one. Used by the byte
  // controller's byobRequest getter (zero-copy is only unambiguous with a
  // single consumer).
  get singleCursor(): QueueCursor<T, V> | undefined {
    if (this.cursorCount !== 1) return undefined;
    let found: QueueCursor<T, V> | undefined;
    this.#forEachLiveCursor((cursor) => {
      found = cursor;
    });
    return found;
  }

  anyCursorHasPendingRead(): boolean {
    let any = false;
    this.#forEachLiveCursor((cursor) => {
      if (cursor.hasPendingRead) any = true;
    });
    return any;
  }

  // The exact "last consumer" test for copy-on-read: true if any OTHER
  // live cursor has not yet advanced past the entry at logicalIndex. When
  // false, the asking cursor is the entry's final consumer and may take
  // the underlying buffer zero-copy.
  hasOtherLiveCursorAtOrBefore(
    cursor: QueueCursor<T, V>,
    logicalIndex: number
  ): boolean {
    let found = false;
    this.#forEachLiveCursor((other) => {
      if (other !== cursor && other.position <= logicalIndex) {
        found = true;
      }
    });
    return found;
  }

  // True if any live cursor satisfies the predicate. Used by the byte
  // controller's close() validation across ALL consumers (tee branches
  // included), not just the single-cursor case.
  someLiveCursor(predicate: (cursor: QueueCursor<T, V>) => boolean): boolean {
    let found = false;
    this.#forEachLiveCursor((cursor) => {
      if (predicate(cursor)) found = true;
    });
    return found;
  }

  // Snapshot of the live owner streams (one per live cursor). Used for
  // error propagation across tee branches — the queue itself stays
  // policy-free; the controller decides what to do with the owners.
  getLiveOwners(): object[] {
    const owners: object[] = [];
    this.#forEachLiveCursor((cursor) => {
      const owner = cursor.ownerDeref();
      if (owner !== undefined) {
        ArrayPrototypePush(owners, owner);
      }
    });
    return owners;
  }

  // Access a slot by logical position. May return the CLOSE_SENTINEL —
  // callers must check (isQueueEntry) before touching value/size.
  getEntry(logicalIndex: number): QueueSlot<T> | undefined {
    const actual = logicalIndex - this.#headOffset;
    if (actual < 0 || actual >= this.#entries.length) return undefined;
    return this.#entries[actual];
  }

  enqueue(entry: QueueEntry<T>, notify: boolean = true): void {
    // The controller pre-checks canCloseOrEnqueue before calling size().
    // A reentrant close()/error() from inside size() may change the state
    // between the pre-check and this push; the spec's EnqueueValueWithSize
    // has no state guard, so we accept the enqueue unconditionally.
    if (this.#state === 'closed') {
      // Reentrant close() from inside size() already pushed the sentinel.
      // The spec's close just sets closeRequested — the chunk is still
      // readable. Insert the entry BEFORE the sentinel so cursors drain
      // through it before reaching the close marker.
      const entries = this.#entries;
      // Sentinel position BEFORE insertion — cursors at or past this
      // point already resolved {done: true} and must not be touched.
      const sentinelPos = this.#headOffset + entries.length - 1;
      const sentinel = entries[entries.length - 1];
      entries[entries.length - 1] = entry;
      ArrayPrototypePush(entries, sentinel as QueueSlot<T>);
      // Only update cursors that haven't yet reached the sentinel.
      // A cursor at sentinelPos already drained and resolved done —
      // inflating its remainingSize or notifying it would corrupt
      // desiredSize and break the drain-then-close terminality guarantee.
      this.#forEachLiveCursor((cursor) => {
        if (cursor.position < sentinelPos) {
          cursor.addToTotalSize(entry.size);
          if (notify) cursor.notify();
        }
      });
    } else {
      ArrayPrototypePush(this.#entries, entry);
      if (this.#state === 'readable') {
        this.#forEachLiveCursor((cursor) => {
          // Increment the cursor's running total BEFORE notify(), which may
          // immediately consume the entry (decrementing it back). The +=/-=
          // order preserves spec-mandated IEEE 754 drift.
          cursor.addToTotalSize(entry.size);
          if (notify) cursor.notify();
        });
      }
    }
  }

  // Push the close sentinel as the final slot and notify. Cursors that have
  // already drained to the sentinel position resolve their pending reads as
  // done; slower cursors discover the close independently when they reach
  // it.
  close(): void {
    if (this.#state !== 'readable') {
      throw new TypeError('Cannot close a closed or errored queue');
    }
    this.#state = 'closed';
    ArrayPrototypePush(this.#entries, CLOSE_SENTINEL);
    this.#forEachLiveCursor((cursor) => {
      cursor.notify();
    });
  }

  // Stream error: reject all pending reads on every cursor (byte cursors
  // also reject pending pull-intos — partial fills are lost) and drop all
  // buffered data. Cursors remain attached; reads submitted after the error
  // are rejected at the reader/stream layer via the stored error.
  error(reason: unknown): void {
    this.#state = 'errored';
    // Capture the logical end BEFORE dropping entries so cursor positions
    // remain meaningful (they all now point at/past the end).
    const end = this.length;
    ArrayPrototypeSplice(this.#entries, 0, this.#entries.length);
    this.#headOffset = end;
    this.#forEachLiveCursor((cursor) => {
      cursor.errorAllReads(reason);
    });
  }

  // Called by the QueueCursor constructor (cursors self-register). `owner`
  // is the stream object — held strongly only by the FinalizationRegistry
  // key and weakly by the cursor. When forking (tee), the new cursor's
  // constructor receives the source cursor's position AND byteOffset so the
  // branch resumes exactly where the original left off.
  addCursor(cursor: QueueCursor<T, V>, owner: object): void {
    this.#hadCursors = true;
    this.#allGoneSignaled = false;
    this.#cursors.add(cursor);
    registerCursorCleanup(owner, cursor);
  }

  removeCursor(cursor: QueueCursor<T, V>): void {
    this.#cursors.delete(cursor);
    unregisterCursorCleanup(cursor);
    this.#gc();
    this.#checkAllCursorsGone();
  }

  // Called whenever any cursor advances: reclaim entries every live cursor
  // has passed. Orphaned cursors are pruned during iteration, so their
  // stale positions never block reclamation.
  onCursorAdvanced(): void {
    this.#gc();
  }

  #gc(): void {
    if (this.#cursors.size === 0) return;
    let minPos = Infinity;
    this.#forEachLiveCursor((cursor) => {
      if (cursor.position < minPos) minPos = cursor.position;
    });
    if (minPos === Infinity) return; // all cursors were orphaned
    const freedCount = minPos - this.#headOffset;
    if (freedCount > 0) {
      ArrayPrototypeSplice(this.#entries, 0, freedCount);
      this.#headOffset = minPos;
    }
  }
}

function registerCursorCleanup(owner: object, cursor: object): void {
  FinalizationRegistryPrototypeRegister(
    cursorCleanupRegistry,
    owner,
    cursor,
    cursor
  );
}

function unregisterCursorCleanup(cursor: object): void {
  FinalizationRegistryPrototypeUnregister(cursorCleanupRegistry, cursor);
}

// ---------------------------------------------------------------------------
// QueueCursor

class QueueCursor<T, V = T> implements StreamConsumer<V> {
  #queue: StreamQueue<T, V>;
  // Weak back-reference to the owning ReadableStream. A cursor whose owner
  // has been GC'd is an orphan and gets pruned by the queue. Note pending
  // reads pin the owner (pending.reader → reader → stream), so orphaned
  // implies no pending reads from any live reader.
  #owner: unknown;
  #position: number;
  #byteOffset: number; // partial consumption of the entry at #position
  #pendingReads: PendingRead<V>[] = [];
  // Running total mirroring the spec's [[queueTotalSize]]. Incremented on
  // enqueue, decremented on consume. Must use +=/-= (not recomputation) to
  // preserve IEEE 754 double-precision drift that WPTs verify.
  #queueTotalSize: number = 0;

  constructor(
    queue: StreamQueue<T, V>,
    owner: object,
    startPosition: number = queue.length,
    byteOffset: number = 0,
    initialTotalSize: number = 0
  ) {
    this.#queue = queue;
    this.#owner = new WeakRef(owner);
    this.#position = startPosition;
    this.#byteOffset = byteOffset;
    this.#queueTotalSize = initialTotalSize;
    queue.addCursor(this, owner);
  }

  get queue(): StreamQueue<T, V> {
    return this.#queue;
  }

  get position(): number {
    return this.#position;
  }

  get byteOffset(): number {
    return this.#byteOffset;
  }

  get hasPendingRead(): boolean {
    return this.#pendingReads.length > 0;
  }

  fulfillFirstPendingRead(value: V): void {
    const pending = ArrayPrototypeShift(this.#pendingReads) as PendingRead<V>;
    pending.resolve(createReadResult(value, false));
  }

  isOrphaned(): boolean {
    return WeakRefPrototypeDeref(this.#owner) === undefined;
  }

  // The owning stream, if it is still alive. Used by the controller to
  // propagate error transitions to every consumer stream (tee branches)
  // without holding strong references to them.
  ownerDeref(): object | undefined {
    return WeakRefPrototypeDeref(this.#owner) as object | undefined;
  }

  // Spec [[queueTotalSize]]: running total of unconsumed entry sizes.
  // Uses += / -= to match IEEE 754 drift that WPTs verify.
  get remainingSize(): number {
    // Clamp to 0 per spec (ResetQueue, EnqueueValueWithSize clamping).
    const total = this.#queueTotalSize;
    return total < 0 ? 0 : total;
  }

  // Called by StreamQueue.enqueue() to increment the running total.
  addToTotalSize(size: number): void {
    this.#queueTotalSize += size;
  }

  // Produce the read-result value for `entry` (which is at the current
  // position). Base implementation: the whole entry value. ByteStreamCursor
  // overrides this to build a remainder view honoring #byteOffset.
  protected readEntryValue(entry: QueueEntry<T>): V {
    return entry.value as unknown as V;
  }

  // Advance past the entry at the current position (whole-entry consume).
  protected advancePastEntry(): void {
    const slot = this.#queue.getEntry(this.#position);
    if (isQueueEntry(slot)) {
      // Only the REMAINING portion of this entry contributes to the
      // running total — bytes before #byteOffset were already debited
      // (by setConsumed or via initialTotalSize at cursor construction).
      this.#queueTotalSize -= slot.size - this.#byteOffset;
    }
    this.#position++;
    this.#byteOffset = 0;
  }

  // Used by byte fills for sub-entry consumption tracking.
  protected setConsumed(position: number, byteOffset: number): void {
    if (position === this.#position && byteOffset === this.#byteOffset) {
      return;
    }
    // Adjust running total for consumed entries and byte offset changes.
    // The entry at the CURRENT position may be partially consumed
    // (#byteOffset > 0) — those bytes were already debited, so only
    // the remainder counts. Subsequent entries are fully outstanding.
    for (let i = this.#position; i < position; i++) {
      const slot = this.#queue.getEntry(i);
      if (isQueueEntry(slot)) {
        this.#queueTotalSize -=
          i === this.#position ? slot.size - this.#byteOffset : slot.size;
      }
    }
    // When staying on the same entry (position unchanged), only the
    // DELTA from old to new offset is freshly consumed. When advancing
    // past entries, the new entry's offset is all-new consumption.
    const alreadyDebited = position === this.#position ? this.#byteOffset : 0;
    this.#queueTotalSize -= byteOffset - alreadyDebited;
    this.#position = position;
    this.#byteOffset = byteOffset;
    this.#queue.onCursorAdvanced();
  }

  // Submit a read. If data is available AND no reads are already pending,
  // fulfill immediately; otherwise defer.
  //
  // INVARIANT (per-reader FIFO): the fast path is taken only when
  // #pendingReads is empty. Entries CAN coexist with pending reads (e.g.
  // while notification is deferred during a batched enqueue), and a fresh
  // read must never jump ahead of older ones.
  //
  // Ordering: spec PullSteps dequeues, then calls CallPullIfNeeded, then
  // fulfills the read request. We preserve that order (advance →
  // onCursorAdvanced, which may trigger a pull → resolve) so the relative
  // microtask ordering of pull side-effects vs. read fulfillment matches
  // ordering-sensitive WPT tests.
  // Attempt a synchronous read. Returns the result directly when data (or
  // the close sentinel) is immediately available at the cursor, or
  // undefined when no data is buffered / reads are already queued. This
  // lets the reader layer perform the drain-then-close check without an
  // intervening microtask (spec PullSteps timing).
  tryReadSync(_reader: object): ReadableStreamReadResult<V> | undefined {
    if (this.#pendingReads.length !== 0) return undefined;
    const slot = this.#queue.getEntry(this.#position);
    if (slot === CLOSE_SENTINEL) {
      return createReadResult(undefined, true);
    }
    if (isQueueEntry(slot)) {
      const value = this.readEntryValue(slot);
      this.advancePastEntry();
      this.#queue.onCursorAdvanced();
      return createReadResult(value, false);
    }
    return undefined;
  }

  read(reader: object): Promise<ReadableStreamReadResult<V>> {
    // Fast path: try the synchronous read first to avoid wrapping in a
    // promise when data is already buffered.
    const sync = this.tryReadSync(reader);
    if (sync !== undefined) return PromiseResolve(sync);
    // No data available (or reads already queued) — defer.
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<
        ReadableStreamReadResult<V>
      >;
    ArrayPrototypePush(this.#pendingReads, { resolve, reject, reader });
    return promise;
  }

  // Called by the queue when new data (or the close sentinel) is enqueued.
  notify(): void {
    while (this.#pendingReads.length > 0) {
      const slot = this.#queue.getEntry(this.#position);
      if (slot === undefined) break;
      if (slot === CLOSE_SENTINEL) {
        // End of stream: every remaining pending read resolves done. This
        // deliberately uses the non-virtual helper — the byte cursor's
        // pending pull-intos must NOT be auto-committed at the sentinel
        // (see "Close semantics for byte cursors" in the design doc).
        this.#resolvePendingReadsAsDone();
        break;
      }
      const entry = slot as QueueEntry<T>;
      const value = this.readEntryValue(entry);
      this.advancePastEntry();
      // assert: pendingReads is non-empty (the while condition guarantees it)
      const pending = ArrayPrototypeShift(this.#pendingReads) as PendingRead<V>;
      pending.resolve(createReadResult(value, false));
    }
    this.#queue.onCursorAdvanced();
  }

  // Reject pending reads submitted by a specific reader (lock release).
  cancelReadsForReader(reader: object, reason: unknown): void {
    const remaining: PendingRead<V>[] = [];
    const pending = this.#pendingReads;
    this.#pendingReads = remaining;
    for (let i = 0; i < pending.length; i++) {
      const read = pending[i] as PendingRead<V>;
      if (read.reader === reader) {
        read.reject(reason);
      } else {
        ArrayPrototypePush(remaining, read);
      }
    }
  }

  // Reject all pending reads (stream ERROR path only).
  errorAllReads(reason: unknown): void {
    this.#queueTotalSize = 0;
    const pending = this.#pendingReads;
    this.#pendingReads = [];
    for (let i = 0; i < pending.length; i++) {
      const read = pending[i] as PendingRead<V>;
      read.reject(reason);
    }
  }

  // Resolve all pending reads as done (stream CANCEL path). Per spec,
  // cancel() RESOLVES pending reads with { done: true, value: undefined } —
  // including BYOB reads, whose partial data is dropped. Rejection is
  // reserved for error() and releaseLock().
  resolveAllReadsAsDone(): void {
    this.#queueTotalSize = 0;
    this.#resolvePendingReadsAsDone();
  }

  // Stream-cancel teardown (StreamConsumer interface). QUEUED INVARIANT:
  // the source-cancel decision runs BEFORE removeCursor so a
  // reason-carrying cancel wins the controller's idempotency cache over
  // the all-cursors-gone GC hook's undefined-reason call. The stream layer
  // owns the policy (tee composite hooks vs direct controller cancel) via
  // `decideSourceCancel` — including binding the reason — while the
  // last-consumer determination is queue knowledge, supplied to it here.
  cancelStream(
    _reason: unknown,
    decideSourceCancel: (isLastConsumer: boolean) => Promise<void>
  ): Promise<void> {
    this.resolveAllReadsAsDone();
    const promise = decideSourceCancel(this.#queue.cursorCount === 1);
    this.#queue.removeCursor(this);
    return promise;
  }

  #resolvePendingReadsAsDone(): void {
    const pending = this.#pendingReads;
    this.#pendingReads = [];
    for (let i = 0; i < pending.length; i++) {
      const read = pending[i] as PendingRead<V>;
      read.resolve(createReadResult(undefined, true));
    }
  }

  // Bulk read for the draining reader (and pipeTo): consume all buffered
  // entries from the current position, up to the soft limit `maxSize`
  // (in strategy size units — bytes for byte streams). Returns
  // synchronously; the draining reader wraps in a promise at its API
  // boundary. Always takes at least one available entry so callers make
  // progress even when maxSize is smaller than the next entry.
  drain(maxSize: number = Infinity): { chunks: V[]; done: boolean } {
    const chunks: V[] = [];
    let total = 0;
    let done = false;
    while (total < maxSize) {
      const slot = this.#queue.getEntry(this.#position);
      if (slot === undefined) break;
      if (slot === CLOSE_SENTINEL) {
        done = true;
        break;
      }
      const entry = slot as QueueEntry<T>;
      ArrayPrototypePush(chunks, this.readEntryValue(entry));
      total += entry.size;
      this.advancePastEntry();
    }
    if (chunks.length > 0) {
      this.#queue.onCursorAdvanced();
    }
    const result = ObjectCreate(null) as { chunks: V[]; done: boolean };
    result.chunks = chunks;
    result.done = done;
    return result;
  }
}

// ---------------------------------------------------------------------------
// ByteStreamCursor

// Byte cursors add sub-entry granularity (partial entry consumption) and
// BYOB support via spec-shaped pull-into descriptors. Entry values are
// {buffer, byteOffset, byteLength} triples; read results are Uint8Array
// remainder views.
class ByteStreamCursor
  extends QueueCursor<ByteQueueEntry, Uint8Array>
  implements ByteStreamConsumer
{
  // Spec [[pendingPullIntos]] — a LIST. Multiple reads can be queued, and
  // autoAllocateChunkSize creates synthetic descriptors for default reads.
  #pendingPullIntos: PullIntoDescriptor[] = [];

  // Callback invoked when the cursor detects a fractional-element fill at
  // the close sentinel — the stream must be errored with a TypeError. Set
  // by the controller (the cursor layer cannot error the stream directly).
  #errorStreamCallback: ((e: unknown) => void) | undefined;

  get hasPendingPullInto(): boolean {
    return this.#pendingPullIntos.length > 0;
  }

  override get hasPendingRead(): boolean {
    if (super.hasPendingRead) return true;
    // Descriptors with readerType 'none' are leftovers from releaseLock —
    // they don't represent active reads and should NOT trigger pull().
    for (let i = 0; i < this.#pendingPullIntos.length; i++) {
      if (
        (this.#pendingPullIntos[i] as PullIntoDescriptor).readerType !== 'none'
      ) {
        return true;
      }
    }
    return false;
  }

  get hasPartiallyFulfilledRead(): boolean {
    const head = this.#pendingPullIntos[0];
    return head !== undefined && head.bytesFilled > 0;
  }

  // The head pull-into descriptor, for the controller's respond() /
  // respondWithNewView() paths (validation, buffer re-transfer and
  // re-pointing happen there, at the trust boundary).
  get headPullInto(): PullIntoDescriptor | undefined {
    return this.#pendingPullIntos[0];
  }

  // View over the unfilled remainder of the head descriptor — what
  // byobRequest.view exposes to the underlying source.
  get pendingPullIntoView(): Uint8Array | undefined {
    const head = this.#pendingPullIntos[0];
    if (head === undefined) return undefined;
    return new Uint8Array(
      head.buffer,
      head.byteOffset + head.bytesFilled,
      head.byteLength - head.bytesFilled
    );
  }

  // Set the callback the controller uses to receive fractional-element-
  // at-close errors (the cursor cannot error the stream directly).
  set errorStreamCallback(cb: (e: unknown) => void) {
    this.#errorStreamCallback = cb;
  }

  // Default reads on a byte cursor return the REMAINDER of the entry at the
  // cursor's position: a view over the original (transferred) entry buffer
  // starting at the current byteOffset (whole entry when byteOffset is 0).
  //
  // COPY-ON-READ for shared entries: if any OTHER live cursor still needs
  // this entry, hand out a copy — aliased mutable views across tee branches
  // would let one consumer corrupt its sibling's data. The last consumer
  // (and the single-cursor common case) takes the view zero-copy. This is
  // the exact last-consumer test from the design doc; readEntryValue is
  // always invoked for the entry at the CURRENT position, before advancing.
  protected override readEntryValue(
    entry: QueueEntry<ByteQueueEntry>
  ): Uint8Array {
    const v = entry.value;
    const view = new Uint8Array(
      v.buffer,
      v.byteOffset + this.byteOffset,
      v.byteLength - this.byteOffset
    );
    if (this.queue.hasOtherLiveCursorAtOrBefore(this, this.position)) {
      return TypedArrayPrototypeSlice(view) as Uint8Array;
    }
    return view;
  }

  // Called by the BYOB reader after validation, buffer transfer, and
  // descriptor construction. Same FIFO invariant as read(): the fast path
  // is taken only when nothing is already pending.
  //
  // PRECONDITION: the stream is readable. Unlike read(), the cursor does
  // not resolve BYOB reads at the sentinel — a read submitted after close
  // must be resolved by the READER layer with { done: true, value:
  // zero-length view over the transferred buffer } before ever reaching
  // here, and errored streams reject at the reader layer. A descriptor
  // pushed here while the queue is already closed would pend until
  // respond(0)/cancel/release, per the byte-cursor close matrix.
  readBYOB(
    desc: PullIntoDescriptor
  ): Promise<ReadableStreamReadResult<ArrayBufferView>> {
    if (this.#pendingPullIntos.length === 0) {
      this.#fillFromQueue(desc);
      if (desc.bytesFilled >= desc.minimumFill) {
        return PromiseResolve(createReadResult(this.#convert(desc), false));
      }
      // After filling, if the cursor is at the close sentinel and the
      // descriptor has a fractional element fill, the remaining bytes can
      // never complete an element — error the stream immediately.
      if (
        desc.bytesFilled > 0 &&
        desc.bytesFilled % desc.elementSize !== 0 &&
        this.queue.getEntry(this.position) === CLOSE_SENTINEL
      ) {
        const e = new TypeError(
          'Insufficient bytes to fill elements in the given view'
        );
        if (this.#errorStreamCallback !== undefined) {
          this.#errorStreamCallback(e);
        }
        return PromiseReject(e);
      }
    }
    ArrayPrototypePush(this.#pendingPullIntos, desc);
    return desc.promise;
  }

  // Keep filling head descriptors from newly queued data while they can be
  // completed; then let the base class service default pending reads
  // (including sentinel handling) and refresh backpressure.
  override notify(): void {
    // Two-phase processing per spec
    // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue:
    // fill ALL ready descriptors first, THEN resolve them. This ensures
    // byobRequest is null by the time any resolve fires (which may trigger
    // user-observable code via Object.prototype.then interception).
    let filledPullIntos:
      | Array<{ desc: PullIntoDescriptor; view: ArrayBufferView }>
      | undefined;
    while (this.#pendingPullIntos.length > 0) {
      const slot = this.queue.getEntry(this.position);
      if (slot === undefined) break;
      if (slot === CLOSE_SENTINEL) {
        // End of DATA. Check for fractional-element fill: if any BYOB
        // descriptor has partially filled bytes that don't align to the
        // element size, the remaining bytes can never complete an element
        // — the stream must be errored with a TypeError (spec
        // ReadableByteStreamControllerClose step 4).
        if (this.#checkFractionalFillAtClose()) return;
        // Synthetic descriptors for DEFAULT reads (autoAllocateChunkSize)
        // follow default-read close semantics: ReadableStreamClose drains
        // read requests with done, so they resolve { done: true } now.
        this.#resolveDefaultPullIntosAsDone();
        // TRUE BYOB descriptors in single-cursor mode are NOT committed
        // here — the source can call respond(0)-while-closed, which
        // reaches commitPullIntosOnClose() via the controller. But in
        // multi-cursor mode (tee branches), byobRequest is null and
        // respond(0) is unreachable, so we must commit them now. This
        // is the equivalent of the spec's per-branch controller running
        // ReadableByteStreamControllerRespondInClosedState.
        if (this.queue.singleCursor === undefined) {
          this.commitPullIntosOnClose();
        }
        break;
      }
      const head = this.#pendingPullIntos[0] as PullIntoDescriptor;
      this.#fillFromQueue(head);
      if (head.bytesFilled < head.minimumFill) break; // need more data
      ArrayPrototypeShift(this.#pendingPullIntos);
      const view = this.#convert(head);
      if (filledPullIntos === undefined) filledPullIntos = [];
      ArrayPrototypePush(filledPullIntos, { desc: head, view });
    }
    // Phase 2: resolve all filled descriptors after the fill loop.
    if (filledPullIntos !== undefined) {
      for (let i = 0; i < filledPullIntos.length; i++) {
        const filled = filledPullIntos[i] as {
          desc: PullIntoDescriptor;
          view: ArrayBufferView;
        };
        filled.desc.resolve(createReadResult(filled.view, false));
      }
    }
    super.notify();
  }

  // Spec ReadableByteStreamControllerClose step 4: if any pending BYOB
  // descriptor has a fractional element fill (bytesFilled > 0 but not
  // element-aligned), the remaining bytes can never form a complete element
  // — error the stream with TypeError and reject all pending reads. Returns
  // true if the stream was errored (caller should bail out of notify).
  #checkFractionalFillAtClose(): boolean {
    for (let i = 0; i < this.#pendingPullIntos.length; i++) {
      const desc = this.#pendingPullIntos[i] as PullIntoDescriptor;
      if (desc.bytesFilled > 0 && desc.bytesFilled % desc.elementSize !== 0) {
        const e = new TypeError(
          'Insufficient bytes to fill elements in the given view'
        );
        if (this.#errorStreamCallback !== undefined) {
          this.#errorStreamCallback(e);
        }
        // errorAllReads is called by the controller's error() path
        // (via the stream error machinery), so we don't call it here.
        return true;
      }
    }
    return false;
  }

  // Resolve synthetic default-read descriptors (autoAllocateChunkSize) as
  // done at end-of-stream; keep true BYOB descriptors pending per the close
  // matrix. In practice the list is homogeneous (single reader type at a
  // time), so the filtering is defensive.
  //
  // The descriptor is KEPT in the list (not shifted) so that a subsequent
  // respond(0) finds it and doesn't throw. commitPullIntosOnClose skips
  // already-resolved descriptors.
  #resolveDefaultPullIntosAsDone(): void {
    for (let i = 0; i < this.#pendingPullIntos.length; i++) {
      const desc = this.#pendingPullIntos[i] as PullIntoDescriptor;
      if (desc.readerType === 'default') {
        desc.resolve(createReadResult(undefined, true));
        desc.readerType = 'none'; // mark as consumed
      }
    }
  }

  // byobRequest.respond(bytesWritten) — zero-copy path. The controller has
  // already validated bytesWritten, re-transferred the buffer, and
  // re-pointed head.buffer at the transferred copy.
  respondBYOB(bytesWritten: number): void {
    const head = this.#pendingPullIntos[0];
    if (head === undefined) {
      throw new TypeError('No pending BYOB request to respond to');
    }
    head.bytesFilled += bytesWritten;

    // Spec ReadableByteStreamControllerRespondInReadableState step 3:
    // if the head descriptor's reader was released (readerType 'none'),
    // the filled data is cloned into the queue rather than resolving a read
    // promise. Subsequent descriptors are then filled from the queue.
    // Shift BEFORE enqueue to avoid re-entrant notify filling the same head.
    if (head.readerType === 'none') {
      ArrayPrototypeShift(this.#pendingPullIntos);
      if (head.bytesFilled > 0) {
        // Clone (not transfer) the filled portion into a new queue entry.
        // enqueue triggers notify() which fills subsequent descriptors.
        this.queue.enqueue({
          value: {
            buffer: ArrayBufferPrototypeSlice(
              head.buffer,
              head.byteOffset,
              head.byteOffset + head.bytesFilled
            ),
            byteOffset: 0,
            byteLength: head.bytesFilled,
          },
          size: head.bytesFilled,
        });
      }
      return;
    }

    if (head.bytesFilled < head.minimumFill) {
      // Not enough yet — stays pending. byobRequest now exposes a view
      // over the unfilled remainder of the (new) buffer; the source can
      // keep writing or fall back to enqueue().
      return;
    }
    // Spec ReadableByteStreamControllerRespondInReadableState step 7–10:
    // Remove from pending FIRST, then split remainder and enqueue.
    // Order matters: enqueue triggers notify() on live cursors, and the
    // head must already be gone to avoid re-entrant filling.
    ArrayPrototypeShift(this.#pendingPullIntos);
    const remainderSize = head.bytesFilled % head.elementSize;
    if (remainderSize > 0) {
      // The remainder bytes live at the END of the filled region.
      const end = head.byteOffset + head.bytesFilled;
      // Enqueue the remainder as a new queue entry (a slice of the
      // transferred buffer). The spec uses CloneArrayBuffer here; we use
      // slice since the buffer is already the transferred copy.
      this.queue.enqueue({
        value: {
          buffer: ArrayBufferPrototypeSlice(
            head.buffer,
            end - remainderSize,
            end
          ),
          byteOffset: 0,
          byteLength: remainderSize,
        },
        size: remainderSize,
      });
      // Truncate bytesFilled to an element-aligned boundary.
      head.bytesFilled -= remainderSize;
    }
    head.resolve(createReadResult(this.#convert(head), false));
    // Spec ProcessPullIntosUsingQueue: data queued via the enqueue path may
    // already satisfy subsequent descriptors — fill them now rather than
    // waiting for the next enqueue. notify() runs exactly that loop (and
    // its default-read/backpressure follow-ups are no-ops here).
    this.notify();
  }

  // Commit all pending pull-into descriptors at end-of-stream: resolve with
  // the filled-so-far view (possibly zero-length — the buffer is handed
  // back) and done: true in a SINGLE result. Called by the controller from
  // the respond(0)-while-closed path. A fractional-element fill never
  // reaches this point: controller.close() throws for it.
  commitPullIntosOnClose(): void {
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = [];
    for (let i = 0; i < pending.length; i++) {
      const desc = pending[i] as PullIntoDescriptor;
      // Skip already-resolved descriptors (auto-allocate close path set
      // readerType to 'none' after resolving the read promise).
      if (desc.readerType === 'none') continue;
      // assert: desc.bytesFilled % desc.elementSize === 0
      desc.resolve(createReadResult(this.#convert(desc), true));
    }
  }

  // Spec ReadableByteStreamControllerEnqueue step 8.5: if the head
  // pending pull-into has readerType 'none' (leftover from releaseLock),
  // transfer its buffer and enqueue any filled data before the new chunk
  // is added. This ensures the released descriptor is processed eagerly.
  drainNoneDescriptors(): void {
    while (this.#pendingPullIntos.length > 0) {
      const head = this.#pendingPullIntos[0] as PullIntoDescriptor;
      if (head.readerType !== 'none') break;
      ArrayPrototypeShift(this.#pendingPullIntos);
      if (head.bytesFilled > 0) {
        // Clone the filled portion into a new queue entry.
        this.queue.enqueue({
          value: {
            buffer: ArrayBufferPrototypeSlice(
              head.buffer,
              head.byteOffset,
              head.byteOffset + head.bytesFilled
            ),
            byteOffset: 0,
            byteLength: head.bytesFilled,
          },
          size: head.bytesFilled,
        });
      }
    }
  }

  // Spec ReadableByteStreamControllerEnqueue step 9.3: if the head
  // descriptor is an auto-allocate (readerType 'default'), shift it out
  // so the controller can fulfill the read directly from the enqueued
  // chunk (bypassing the queue and the auto-allocate buffer).
  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined {
    if (this.#pendingPullIntos.length === 0) return undefined;
    const head = this.#pendingPullIntos[0] as PullIntoDescriptor;
    if (head.readerType !== 'default') return undefined;
    ArrayPrototypeShift(this.#pendingPullIntos);
    return head;
  }

  // Stream error: partial fills are lost.
  override errorAllReads(reason: unknown): void {
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = [];
    for (let i = 0; i < pending.length; i++) {
      const desc = pending[i] as PullIntoDescriptor;
      desc.reject(reason);
    }
    super.errorAllReads(reason);
  }

  // Stream cancel: pending BYOB reads resolve { done: true, value:
  // undefined } — partial data and buffers are dropped (spec,
  // WPT-verified).
  override resolveAllReadsAsDone(): void {
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = [];
    for (let i = 0; i < pending.length; i++) {
      const desc = pending[i] as PullIntoDescriptor;
      desc.resolve(createReadResult(undefined, true));
    }
    super.resolveAllReadsAsDone();
  }

  // Reject pull-intos submitted by a specific reader (lock release).
  override cancelReadsForReader(reader: object, reason: unknown): void {
    // Spec: on releaseLock, the reader's readIntoRequests are rejected, but
    // the controller's pendingPullIntos STAY. Descriptors whose reader is
    // being released have their readerType set to 'none' — respond() will
    // then enqueue the filled data instead of resolving a read promise.
    // The byobRequest is NOT invalidated (it still points at the head
    // descriptor's buffer).
    for (let i = 0; i < this.#pendingPullIntos.length; i++) {
      const desc = this.#pendingPullIntos[i] as PullIntoDescriptor;
      if (desc.reader === reader) {
        desc.reject(reason);
        desc.readerType = 'none';
      }
    }
    super.cancelReadsForReader(reader, reason);
  }

  // The ONLY place result views are built. Construction count is in
  // ELEMENTS (bytesFilled / elementSize) — passing bytes happens to work
  // for Uint8Array and is wrong for every other view type. For DataView,
  // elementSize is 1 and its length parameter is in bytes, so the same
  // expression holds.
  #convert(desc: PullIntoDescriptor): ArrayBufferView {
    // assert: desc.bytesFilled % desc.elementSize === 0
    // assert: desc.bytesFilled <= desc.byteLength
    return ReflectConstruct(desc.viewCtor, [
      desc.buffer,
      desc.byteOffset,
      desc.bytesFilled / desc.elementSize,
    ]);
  }

  // Total bytes available to this cursor in the queue (up to the sentinel),
  // accounting for the partially consumed current entry.
  #availableBytes(): number {
    let available = 0;
    for (let i = this.position; i < this.queue.length; i++) {
      const slot = this.queue.getEntry(i);
      if (!isQueueEntry(slot)) break; // sentinel is always last
      available += slot.value.byteLength;
    }
    return available - this.byteOffset;
  }

  // Fill `desc` from the queue starting at (position, byteOffset). Mirrors
  // ReadableByteStreamControllerFillPullIntoDescriptorFromQueue:
  //   - if queued data reaches an element-aligned boundary >= minimumFill,
  //     copy only up to that aligned boundary (remainder bytes stay queued
  //     for the next read) — descriptor is then ready;
  //   - otherwise copy everything available (the descriptor may temporarily
  //     end mid-element) and stay pending.
  // Stops at the CLOSE_SENTINEL. Advances position/byteOffset for consumed
  // bytes (which triggers GC + backpressure refresh via setConsumed).
  #fillFromQueue(desc: PullIntoDescriptor): void {
    const available = this.#availableBytes();
    if (available <= 0) return;

    const maxBytesToCopy = MathMin(
      available,
      desc.byteLength - desc.bytesFilled
    );
    const maxBytesFilled = desc.bytesFilled + maxBytesToCopy;
    const maxAlignedBytes =
      maxBytesFilled - (maxBytesFilled % desc.elementSize);
    let remaining =
      maxAlignedBytes >= desc.minimumFill
        ? maxAlignedBytes - desc.bytesFilled
        : maxBytesToCopy;

    let pos = this.position;
    let off = this.byteOffset;
    while (remaining > 0) {
      // Guaranteed to be a data entry by the #availableBytes computation.
      const entry = (this.queue.getEntry(pos) as QueueEntry<ByteQueueEntry>)
        .value;
      const n = MathMin(entry.byteLength - off, remaining);
      const dest = new Uint8Array(
        desc.buffer,
        desc.byteOffset + desc.bytesFilled,
        n
      );
      const src = new Uint8Array(entry.buffer, entry.byteOffset + off, n);
      TypedArrayPrototypeSet(dest, src);
      desc.bytesFilled += n;
      remaining -= n;
      off += n;
      if (off === entry.byteLength) {
        pos++;
        off = 0;
      }
    }
    this.setConsumed(pos, off);
  }
}

// Type-only exports (fully erased at runtime — the loader sees only the
// module.exports assignment below, matching the readable.ts pattern).
export type { StreamQueue, QueueCursor, ByteStreamCursor };

module.exports = {
  CLOSE_SENTINEL,
  createReadResult,
  StreamQueue,
  QueueCursor,
  ByteStreamCursor,
};
