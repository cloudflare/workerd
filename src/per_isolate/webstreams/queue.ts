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
//     propagation mechanism (drain-then-close per cursor). BYOB
//     descriptors facing the sentinel settle via a DEFERRED end-of-data
//     settlement (one microtask): a same-turn respond(0) first claims the
//     spec's RespondInClosedState fold (done: true with the partial bytes);
//     otherwise the read settles with the C++-parity tail shape —
//     element-aligned partial fills resolve { done: false, value: partial }
//     and unfilled descriptors resolve { done: true, value: empty view }.
//     The result owns the descriptor's buffer (transferred once, no copy);
//     the descriptor remains listed, flagged settledAtEndOfData, so a later
//     respond(0) is still legal and knows not to transfer again. (The spec
//     leaves the read itself pending until that response — a footgun we
//     deliberately do not reproduce.)
//   - read() takes the fast path ONLY when no reads are pending (per-reader
//     FIFO; entries and pending reads coexist under batched notification).
//   - Byte entries are {buffer, byteOffset, byteLength} triples; queue
//     internals never read view metadata through patchable getters.
//   - Copy-on-read for shared byte entries: copy iff any OTHER live cursor
//     still needs the entry (exact last-consumer test) — REQUIRED for
//     soundness, not just spec parity.
//   - Cursors hold weak owner refs; orphan pruning happens on every cursor
//     walk (the controller's own cursor excepted: the controller pins its
//     owner), with the FinalizationRegistry as the idle-queue backstop
//     (the native backend needs NEITHER — JSG owns its source lifetime).
//   - A queue whose last cursor has left or been collected has no consumer
//     for good: it drops what it holds and what is enqueued later, and
//     desiredSize reads as the high-water mark. The controller releases the
//     source (no more pulls; cancel never runs — GC timing runs no user
//     callback) but keeps its own state machine.
//   - desiredSize reflects the SLOWEST live cursor; a pending read on any
//     cursor overrides backpressure at the controller.
//   - A released reader's filled bytes precede later data. respond() on the
//     released head re-queues them; enqueue() keeps them in the byte
//     cursor's own prefix, ahead of the chunk, before any read is served,
//     so a pending read(view) takes both (spec steps 8.5 and 10). Forks
//     (tee, detach) copy them.
//
// Nothing in this module is ever exposed to user code: queues and cursors
// are held in #private fields of the stream classes. Method calls on these
// classes still follow the bootstrap primordials discipline (no bare
// prototype lookups on builtins, no for...of over Sets/arrays).

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  ReadableStreamReadResult,
} from './types';
import type {
  RingBuffer as RingBufferType,
  RingBufferConstructor,
} from './ring-buffer';

const {
  ArrayBuffer,
  ArrayBufferPrototypeByteLengthGet,
  ArrayBufferPrototypeTransferToFixedLength,
  ArrayPrototypePush,
  FinalizationRegistry,
  FinalizationRegistryPrototypeRegister,
  FinalizationRegistryPrototypeUnregister,
  MathMin,
  ObjectCreate,
  PromisePrototypeThen,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  ReflectConstruct,
  Symbol,
  TypeError,
  TypedArrayPrototypeSet,
  Uint8Array,
  WeakRef,
  WeakRefPrototypeDeref,
} = primordials;

const { RingBuffer } = require('webstreams/ring-buffer') as {
  RingBuffer: RingBufferConstructor;
};

// Read-result objects are plain { value, done } objects with the default
// Object.prototype, as the spec requires. Resolving a read promise with one
// looks up `then` on it, so a patched Object.prototype.then can intercept
// the user's read and the internal code that consumes the same promise
// (the pipe, the draining fallback, drain-then-close). Accepted: the
// spec's own resolution has the same lookup.
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

// Spec CloneArrayBuffer: a fresh %ArrayBuffer% holding the given bytes.
// ArrayBuffer.prototype.slice would consult the (user-patchable) species
// constructor instead.
function cloneArrayBuffer(
  buffer: ArrayBuffer,
  byteOffset: number,
  byteLength: number
): ArrayBuffer {
  const copy = new ArrayBuffer(byteLength);
  TypedArrayPrototypeSet(
    new Uint8Array(copy),
    new Uint8Array(buffer, byteOffset, byteLength)
  );
  return copy;
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

// Errors the stream owning a byte cursor (see ByteStreamCursor's
// errorStreamCallback); `owner` is undefined once it has been collected.
export type ErrorStreamCallback = (
  e: unknown,
  owner: object | undefined
) => void;

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
  // 'none' = descriptor whose reader was released (releaseLock), or whose
  // read was settled at end-of-data. In readable state, respond() enqueues
  // the data rather than resolving a read promise.
  readerType: 'byob' | 'default' | 'none';
  // Set when the deferred end-of-data settlement resolved this read: the
  // result view was built over `buffer` (transferred once, at settlement),
  // so the reader owns those bytes. The descriptor stays listed so a later
  // closed-state respond(0)/respondWithNewView(empty) remains legal, but
  // those paths MUST NOT re-transfer `buffer` — it would detach the
  // delivered result. Independent of readerType: a releaseLock after
  // settlement rewrites readerType to 'none' and must not lose this.
  settledAtEndOfData: boolean;
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
  // Spec step 9.3: if the head descriptor is an auto-allocate
  // (readerType 'default'), shift it out and return it so the controller
  // can fulfill the read directly from the enqueued chunk.
  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined;
}

// ---------------------------------------------------------------------------
// Orphan-detection backstop
//
// Lazy WeakRef pruning (see StreamQueue#prune) only runs inside queue
// operations. An idle queue whose consumers were all GC'd — but which is
// pinned from the C++ side via the controller — would otherwise never
// observe "all cursors gone" and never cancel the underlying source. The
// registry provides the active signal; cleanup scheduling is controlled by
// workerd and deliberately coarse.

interface CursorLike {
  queue: { removeCursor(cursor: unknown): void };
}

const cursorCleanupRegistry = new FinalizationRegistry((cursorRef: unknown) => {
  const cursor = WeakRefPrototypeDeref(cursorRef) as CursorLike | undefined;
  if (cursor !== undefined) {
    cursor.queue.removeCursor(cursor);
  }
});

// ---------------------------------------------------------------------------
// StreamQueue

// T is the entry value type (ByteQueueEntry for byte streams); V is the type
// delivered to read results (Uint8Array remainder views for byte streams).
class StreamQueue<T, V = T> {
  #entries: RingBufferType<QueueSlot<T>> = new RingBuffer();
  #headOffset: number = 0; // logical index of the entries' head
  // Live cursors in join order, walked by index; almost always exactly one.
  #cursors: QueueCursor<T, V>[] = [];
  // The controller's own cursor. Its owner is the controller's stream,
  // which the controller holds, so it cannot be orphaned while anything
  // can reach this queue; #prune skips its deref. Tee and detach remove it,
  // and every cursor they create can be orphaned: a detached shell adopts
  // the controller, but the controller's stream stays the husk, so nothing
  // strong holds the shell. Its cursor therefore keeps the per-walk deref
  // (the C++ bridge's extraction path, e.g. a JS stream given to Response).
  #anchor: QueueCursor<T, V> | undefined;
  #highWaterMark: number;
  #state: 'readable' | 'closed' | 'errored' = 'readable';
  // Idempotent hook, wired to the controller: releases the underlying source
  // when the last consumer goes away. "cursorCount === 0" is a state, not an
  // event — every path that can remove the last cursor funnels here.
  #onAllCursorsGone: () => void;
  #hadCursors: boolean = false;
  // An internal source's notification that consumption progressed: called
  // at the end of every reclaim walk (a cursor advanced or left), so the
  // slowest cursor's backlog may have shrunk. It must run no user code
  // (it is called inside the walk); see setConsumptionHook.
  #onConsumption: (() => void) | undefined;
  // Set once every cursor has left or been collected. No cursor can join
  // afterwards (one is only ever forked from a live one), so nothing will
  // read the queue again: it drops what it holds and what is enqueued later.
  #noConsumers: boolean = false;

  constructor(highWaterMark: number, onAllCursorsGone: () => void) {
    this.#highWaterMark = highWaterMark;
    this.#onAllCursorsGone = onAllCursorsGone;
  }

  // Drop cursors whose owning stream has been collected. Every walk starts
  // here, so a stale position never blocks reclamation or holds
  // backpressure. #cursors is stable for the rest of a walk whose callbacks
  // run no user code (all but notify(), see #notifyAll): they never add or
  // remove a cursor synchronously, and a re-entrant prune (notify → #gc)
  // finds nothing new, since a deref'd owner stays alive to the end of the
  // job.
  #prune(): void {
    const cursors = this.#cursors;
    for (let i = cursors.length - 1; i >= 0; i--) {
      const cursor = cursors[i] as QueueCursor<T, V>;
      if (cursor !== this.#anchor && cursor.isOrphaned()) {
        this.#removeAt(i);
        unregisterCursorCleanup(cursor);
      }
    }
    this.#checkAllCursorsGone();
  }

  #removeAt(index: number): void {
    const cursors = this.#cursors;
    const last = cursors.length - 1;
    for (let j = index; j < last; j++) {
      cursors[j] = cursors[j + 1] as QueueCursor<T, V>;
    }
    cursors.length = last;
  }

  #checkAllCursorsGone(): void {
    if (this.#hadCursors && !this.#noConsumers && this.#cursors.length === 0) {
      this.#noConsumers = true;
      this.#headOffset += this.#entries.length;
      this.#entries.clear();
      this.#onAllCursorsGone();
    }
  }

  // The slowest cursor's backlog determines backpressure. Note that a
  // pending read on any cursor overrides backpressure at the controller
  // (shouldPull), so this is honest signaling, not a memory bound. With
  // every consumer gone it reads as the high-water mark, as for consumers
  // that keep up.
  get desiredSize(): number {
    this.#prune();
    const cursors = this.#cursors;
    let max = 0;
    for (let i = 0; i < cursors.length; i++) {
      const remaining = (cursors[i] as QueueCursor<T, V>).remainingSize;
      if (remaining > max) max = remaining;
    }
    return this.#highWaterMark - max;
  }

  // The controller's pull condition in one walk: a consumer remains, and
  // either the slowest cursor is below the high-water mark or a read is
  // waiting on some cursor (which overrides backpressure).
  wantsPull(): boolean {
    this.#prune();
    if (this.#noConsumers) return false;
    const cursors = this.#cursors;
    let max = 0;
    for (let i = 0; i < cursors.length; i++) {
      const cursor = cursors[i] as QueueCursor<T, V>;
      if (cursor.hasPendingRead) return true;
      const remaining = cursor.remainingSize;
      if (remaining > max) max = remaining;
    }
    return this.#highWaterMark - max > 0;
  }

  get length(): number {
    // Logical end position (one past the last slot).
    return this.#headOffset + this.#entries.length;
  }

  get cursorCount(): number {
    this.#prune();
    return this.#cursors.length;
  }

  // The single live cursor, if there is exactly one. Used by the byte
  // controller's byobRequest getter (zero-copy is only unambiguous with a
  // single consumer).
  get singleCursor(): QueueCursor<T, V> | undefined {
    this.#prune();
    const cursors = this.#cursors;
    return cursors.length === 1 ? cursors[0] : undefined;
  }

  // The exact "last consumer" test for copy-on-read: true if any OTHER
  // live cursor has not yet advanced past the entry at logicalIndex. When
  // false, the asking cursor is the entry's final consumer and may take
  // the underlying buffer zero-copy.
  hasOtherLiveCursorAtOrBefore(
    cursor: QueueCursor<T, V>,
    logicalIndex: number
  ): boolean {
    this.#prune();
    const cursors = this.#cursors;
    for (let i = 0; i < cursors.length; i++) {
      const other = cursors[i] as QueueCursor<T, V>;
      if (other !== cursor && other.position <= logicalIndex) return true;
    }
    return false;
  }

  forEachLiveCursor(fn: (cursor: QueueCursor<T, V>) => void): void {
    this.#prune();
    const cursors = this.#cursors;
    for (let i = 0; i < cursors.length; i++) {
      fn(cursors[i] as QueueCursor<T, V>);
    }
  }

  // True if any live cursor satisfies the predicate. Used by the byte
  // controller's close() validation across ALL consumers (tee branches
  // included), not just the single-cursor case.
  someLiveCursor(predicate: (cursor: QueueCursor<T, V>) => boolean): boolean {
    this.#prune();
    const cursors = this.#cursors;
    for (let i = 0; i < cursors.length; i++) {
      if (predicate(cursors[i] as QueueCursor<T, V>)) return true;
    }
    return false;
  }

  // Snapshot of the live owner streams (one per live cursor). Used for
  // error propagation across tee branches — the queue itself stays
  // policy-free; the controller decides what to do with the owners.
  getLiveOwners(): object[] {
    this.#prune();
    const cursors = this.#cursors;
    const owners: object[] = [];
    for (let i = 0; i < cursors.length; i++) {
      const owner = (cursors[i] as QueueCursor<T, V>).ownerDeref();
      if (owner !== undefined) ArrayPrototypePush(owners, owner);
    }
    return owners;
  }

  // Access a slot by logical position. May return the CLOSE_SENTINEL —
  // callers must check (isQueueEntry) before touching value/size.
  getEntry(logicalIndex: number): QueueSlot<T> | undefined {
    return this.#entries.get(logicalIndex - this.#headOffset);
  }

  enqueue(entry: QueueEntry<T>, notify: boolean = true): void {
    if (this.#noConsumers) return;
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
      const sentinel = entries.pop() as QueueSlot<T>;
      entries.push(entry);
      entries.push(sentinel);
      // Only update cursors that haven't yet reached the sentinel.
      // A cursor at sentinelPos already drained and resolved done —
      // inflating its remainingSize or notifying it would corrupt
      // desiredSize and break the drain-then-close terminality guarantee.
      this.#prune();
      const cursors = this.#cursors;
      const behind: QueueCursor<T, V>[] = [];
      for (let i = 0; i < cursors.length; i++) {
        const cursor = cursors[i] as QueueCursor<T, V>;
        if (cursor.position < sentinelPos) {
          cursor.addToTotalSize(entry.size);
          ArrayPrototypePush(behind, cursor);
        }
      }
      if (notify) this.#notifyEach(behind);
    } else {
      this.#entries.push(entry);
      if (this.#state === 'readable') {
        this.#prune();
        const cursors = this.#cursors;
        // Increment every cursor's running total BEFORE any notify(), which
        // may immediately consume the entry (decrementing it back). The
        // +=/-= order preserves spec-mandated IEEE 754 drift, and a branch
        // forked inside a notify() inherits a total that counts the entry.
        for (let i = 0; i < cursors.length; i++) {
          (cursors[i] as QueueCursor<T, V>).addToTotalSize(entry.size);
        }
        if (notify) this.#notifyAll();
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
    if (this.#noConsumers) return;
    this.#entries.push(CLOSE_SENTINEL);
    this.#prune();
    this.#notifyAll();
  }

  // notify() is the one walk callback that runs user code: it resolves read
  // promises with plain { value, done } objects, whose `then` lookup invokes
  // a patched Object.prototype.then getter synchronously, and that getter
  // can cancel or tee a branch, removing its cursor and shifting the tail of
  // #cursors down. A lone cursor leaves nothing to skip; with more, notify a
  // copy: a cursor that left meanwhile has no pending reads, so its notify()
  // is a no-op, and one that joined is a fresh branch with none.
  #notifyAll(): void {
    const cursors = this.#cursors;
    if (cursors.length === 1) {
      (cursors[0] as QueueCursor<T, V>).notify();
      return;
    }
    const snapshot: QueueCursor<T, V>[] = [];
    for (let i = 0; i < cursors.length; i++) {
      ArrayPrototypePush(snapshot, cursors[i] as QueueCursor<T, V>);
    }
    this.#notifyEach(snapshot);
  }

  // `cursors` must not alias #cursors (see #notifyAll).
  #notifyEach(cursors: QueueCursor<T, V>[]): void {
    for (let i = 0; i < cursors.length; i++) {
      (cursors[i] as QueueCursor<T, V>).notify();
    }
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
    this.#entries.clear();
    this.#headOffset = end;
    this.#prune();
    const cursors = this.#cursors;
    for (let i = 0; i < cursors.length; i++) {
      (cursors[i] as QueueCursor<T, V>).errorAllReads(reason);
    }
  }

  // Called by the QueueCursor constructor (cursors self-register). `owner`
  // is the stream object — registered as the FinalizationRegistry's weak
  // target and also held weakly by the cursor. When forking (tee), the new
  // cursor's constructor receives the source cursor's position AND byteOffset
  // so the branch resumes exactly where the original left off.
  //
  // A cursor joins a queue that already has one only through tee or detach,
  // which then remove the parent's. The controllers rely on that: while the
  // source's own cursor is present it is the queue's sole consumer
  // (#maybeCloseStream in readable.ts skips the owners walk). A new path
  // that adds a cursor beside the source's own must revisit those checks.
  addCursor(cursor: QueueCursor<T, V>, owner: object): void {
    this.#hadCursors = true;
    ArrayPrototypePush(this.#cursors, cursor);
    registerCursorCleanup(owner, cursor);
  }

  // Marks the controller's own cursor (see #anchor).
  anchorCursor(cursor: QueueCursor<T, V>): void {
    this.#anchor = cursor;
  }

  removeCursor(cursor: QueueCursor<T, V>): void {
    const cursors = this.#cursors;
    for (let i = 0; i < cursors.length; i++) {
      if (cursors[i] === cursor) {
        this.#removeAt(i);
        break;
      }
    }
    if (cursor === this.#anchor) this.#anchor = undefined;
    unregisterCursorCleanup(cursor);
    this.#gc();
  }

  // Called whenever any cursor advances: reclaim entries every live cursor
  // has passed. Orphaned cursors are pruned first, so their stale positions
  // never block reclamation.
  onCursorAdvanced(): void {
    this.#gc();
  }

  // Installs (or clears) the consumption notification. The identity
  // streams settle a write once the slowest consumer has read past it.
  setConsumptionHook(hook: (() => void) | undefined): void {
    this.#onConsumption = hook;
  }

  #gc(): void {
    this.#prune();
    const cursors = this.#cursors;
    if (cursors.length === 0) return;
    let minPos = (cursors[0] as QueueCursor<T, V>).position;
    for (let i = 1; i < cursors.length; i++) {
      const position = (cursors[i] as QueueCursor<T, V>).position;
      if (position < minPos) minPos = position;
    }
    const freedCount = minPos - this.#headOffset;
    if (freedCount > 0) {
      this.#entries.trimFront(freedCount);
      this.#headOffset = minPos;
    }
    const hook = this.#onConsumption;
    if (hook !== undefined) hook();
  }
}

function registerCursorCleanup(owner: object, cursor: object): void {
  FinalizationRegistryPrototypeRegister(
    cursorCleanupRegistry,
    owner,
    // Held values are strongly retained, so retain only a weak reference
    // to the cursor. A live queue keeps its registered cursors alive.
    new WeakRef(cursor),
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
  #pendingReads: RingBufferType<PendingRead<V>> = new RingBuffer();
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
    const pending = this.#pendingReads.shift() as PendingRead<V>;
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
    this.#pendingReads.push({ resolve, reject, reader });
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
      const pending = this.#pendingReads.shift() as PendingRead<V>;
      pending.resolve(createReadResult(value, false));
    }
    this.#queue.onCursorAdvanced();
  }

  // Reject pending reads submitted by a specific reader (lock release).
  cancelReadsForReader(reader: object, reason: unknown): void {
    const remaining: RingBufferType<PendingRead<V>> = new RingBuffer();
    const pending = this.#pendingReads;
    this.#pendingReads = remaining;
    for (let i = 0; i < pending.length; i++) {
      const read = pending.get(i) as PendingRead<V>;
      if (read.reader === reader) {
        read.reject(reason);
      } else {
        remaining.push(read);
      }
    }
  }

  // Reject all pending reads (stream ERROR path only).
  errorAllReads(reason: unknown): void {
    this.#queueTotalSize = 0;
    const pending = this.#pendingReads;
    this.#pendingReads = new RingBuffer();
    for (let i = 0; i < pending.length; i++) {
      const read = pending.get(i) as PendingRead<V>;
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
  // the source-cancel decision runs BEFORE removeCursor: removing the last
  // cursor first would fire the all-cursors-gone hook, which releases the
  // cancel algorithm before the decision could run it. The stream layer
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
    this.#pendingReads = new RingBuffer();
    for (let i = 0; i < pending.length; i++) {
      const read = pending.get(i) as PendingRead<V>;
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
  #pendingPullIntos: RingBufferType<PullIntoDescriptor> = new RingBuffer();

  // A released head's filled bytes (see flushReleasedHead), cursor-local
  // since the queue may be shared. Read before the data at the cursor's
  // position, and counted in remainingSize. Empty whenever a pull-into is
  // pending (fills take it first).
  #prefix: ByteQueueEntry | undefined;

  // One-shot latch for the deferred end-of-data settlement (see
  // #scheduleEndOfDataSettlement).
  #endOfDataSettlementScheduled: boolean = false;

  // Callback invoked when the cursor detects a fractional-element fill at
  // the close sentinel — the cursor's stream must be errored with a
  // TypeError. Set by the stream layer (the cursor layer cannot error a
  // stream directly); it receives the cursor's owner, held weakly here.
  #errorStreamCallback: ErrorStreamCallback | undefined;

  get hasPendingPullInto(): boolean {
    return this.#pendingPullIntos.length > 0;
  }

  override get hasPendingRead(): boolean {
    if (super.hasPendingRead) return true;
    // Descriptors with readerType 'none' are leftovers from releaseLock or
    // end-of-data settlement. They don't represent active reads and should
    // NOT trigger pull().
    const pending = this.#pendingPullIntos;
    for (let i = 0; i < pending.length; i++) {
      if ((pending.get(i) as PullIntoDescriptor).readerType !== 'none') {
        return true;
      }
    }
    return false;
  }

  get hasPartiallyFulfilledRead(): boolean {
    const head = this.#pendingPullIntos.peek();
    return head !== undefined && head.bytesFilled > 0;
  }

  // The head pull-into descriptor, for the controller's respond() /
  // respondWithNewView() paths (validation, buffer re-transfer and
  // re-pointing happen there, at the trust boundary).
  get headPullInto(): PullIntoDescriptor | undefined {
    return this.#pendingPullIntos.peek();
  }

  // View over the unfilled remainder of the head descriptor — what
  // byobRequest.view exposes to the underlying source.
  get pendingPullIntoView(): Uint8Array | undefined {
    const head = this.#pendingPullIntos.peek();
    if (head === undefined) return undefined;
    return new Uint8Array(
      head.buffer,
      head.byteOffset + head.bytesFilled,
      head.byteLength - head.bytesFilled
    );
  }

  // The callback through which the stream layer receives fractional-
  // element-at-close errors. A cursor moved to a new owner (detach) keeps
  // its predecessor's.
  get errorStreamCallback(): ErrorStreamCallback | undefined {
    return this.#errorStreamCallback;
  }

  set errorStreamCallback(cb: ErrorStreamCallback | undefined) {
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
    const byteOffset = v.byteOffset + this.byteOffset;
    const byteLength = v.byteLength - this.byteOffset;
    if (this.queue.hasOtherLiveCursorAtOrBefore(this, this.position)) {
      return new Uint8Array(cloneArrayBuffer(v.buffer, byteOffset, byteLength));
    }
    return new Uint8Array(v.buffer, byteOffset, byteLength);
  }

  // The prefix is delivered whole, zero-copy (no other cursor holds it).
  #takePrefix(): Uint8Array {
    const prefix = this.#prefix as ByteQueueEntry;
    this.#prefix = undefined;
    this.addToTotalSize(-prefix.byteLength);
    return new Uint8Array(prefix.buffer, prefix.byteOffset, prefix.byteLength);
  }

  // Spec EnqueueDetachedPullIntoToQueue, into the prefix: see #prefix.
  #moveToPrefix(desc: PullIntoDescriptor): void {
    if (desc.bytesFilled === 0) return;
    // A pull-into still pending has taken every available byte, the prefix
    // first, so a released head holding bytes and a prefix never coexist.
    // Were both present, their order would be unknown: fail loudly instead
    // of losing or reordering bytes.
    if (this.#prefix !== undefined) {
      throw new TypeError(
        'ReadableStream internal error: released bytes would replace undelivered bytes'
      );
    }
    this.#prefix = {
      buffer: cloneArrayBuffer(desc.buffer, desc.byteOffset, desc.bytesFilled),
      byteOffset: 0,
      byteLength: desc.bytesFilled,
    };
    this.addToTotalSize(desc.bytesFilled);
  }

  override tryReadSync(
    reader: object
  ): ReadableStreamReadResult<Uint8Array> | undefined {
    if (this.#prefix !== undefined && !super.hasPendingRead) {
      return createReadResult(this.#takePrefix(), false);
    }
    return super.tryReadSync(reader);
  }

  override drain(maxSize: number = Infinity): {
    chunks: Uint8Array[];
    done: boolean;
  } {
    if (this.#prefix === undefined) return super.drain(maxSize);
    const first = this.#takePrefix();
    const result = super.drain(maxSize - first.byteLength);
    const chunks: Uint8Array[] = [first];
    for (let i = 0; i < result.chunks.length; i++) {
      ArrayPrototypePush(chunks, result.chunks[i] as Uint8Array);
    }
    result.chunks = chunks;
    return result;
  }

  // enqueue() step 8.5: a released head's filled bytes move to the
  // prefix, ahead of the chunk being enqueued. Pending read(view)s are
  // left for the enqueue's notify(), which fills them from both (step
  // 10); a pending auto-allocated default read takes the bytes alone now
  // (step 9.1), before the chunk can reach it.
  flushReleasedHead(): void {
    const head = this.#pendingPullIntos.peek();
    if (head !== undefined && head.readerType === 'none') {
      this.#pendingPullIntos.shift();
      this.#moveToPrefix(head);
    }
    const desc = this.#pendingPullIntos.peek();
    const view = this.#takePrefixForDefaultPullInto();
    if (view !== undefined) {
      (desc as PullIntoDescriptor).resolve(createReadResult(view, false));
    }
  }

  // A default read waiting on an auto-allocated descriptor takes the
  // prefix whole, as it would a queued entry (spec
  // FillReadRequestFromQueue), rather than a copy into its buffer. Shifts
  // the head descriptor and returns the prefix for the caller to resolve
  // it with.
  #takePrefixForDefaultPullInto(): Uint8Array | undefined {
    const head = this.#pendingPullIntos.peek();
    if (
      this.#prefix === undefined ||
      head === undefined ||
      head.readerType !== 'default'
    ) {
      return undefined;
    }
    this.#pendingPullIntos.shift();
    return this.#takePrefix();
  }

  // The controller's released head (see its #releasedHead) was responded
  // to: it enqueues those bytes itself, so this cursor's copy of them
  // (from adoptReleasedBytes) goes without being delivered.
  dropReleasedHead(): void {
    const head = this.#pendingPullIntos.peek();
    if (head !== undefined && head.readerType === 'none') {
      this.#pendingPullIntos.shift();
    }
  }

  // A cursor forked from `from` (tee, detach) copies its undelivered
  // released bytes: the prefix, and a released head holding bytes.
  adoptReleasedBytes(from: ByteStreamCursor): void {
    const prefix = from.#prefix;
    if (prefix !== undefined) {
      // Already counted in the remainingSize this cursor started with.
      this.#prefix = {
        buffer: cloneArrayBuffer(
          prefix.buffer,
          prefix.byteOffset,
          prefix.byteLength
        ),
        byteOffset: 0,
        byteLength: prefix.byteLength,
      };
    }
    const head = from.#pendingPullIntos.peek();
    if (
      head !== undefined &&
      head.readerType === 'none' &&
      head.bytesFilled > 0
    ) {
      this.#pendingPullIntos.push({
        buffer: cloneArrayBuffer(
          head.buffer,
          0,
          ArrayBufferPrototypeByteLengthGet(head.buffer)
        ),
        bufferByteLength: head.bufferByteLength,
        byteOffset: head.byteOffset,
        byteLength: head.byteLength,
        bytesFilled: head.bytesFilled,
        minimumFill: head.minimumFill,
        elementSize: head.elementSize,
        viewCtor: head.viewCtor,
        readerType: 'none',
        settledAtEndOfData: head.settledAtEndOfData,
        promise: head.promise,
        resolve: head.resolve,
        reject: head.reject,
        reader: head.reader,
      });
    }
  }

  // Called by the BYOB reader after validation, buffer transfer, and
  // descriptor construction. Same FIFO invariant as read(): the fast path
  // is taken only when nothing is already pending.
  //
  // PRECONDITION: the owning stream is readable. A read submitted after
  // the STREAM state flips to closed is resolved by the READER layer with
  // { done: true, value: zero-length view over the transferred buffer }
  // before ever reaching here, and errored streams reject at the reader
  // layer. The stream can still be readable while the QUEUE is already
  // closed (buffered data not yet drained — e.g. a tee branch that has
  // not read); a below-minimum fill that lands at the sentinel then
  // settles via the deferred end-of-data settlement.
  readBYOB(
    desc: PullIntoDescriptor
  ): Promise<ReadableStreamReadResult<ArrayBufferView>> {
    if (this.#pendingPullIntos.length === 0) {
      this.#fillFromQueue(desc);
      if (desc.bytesFilled >= desc.minimumFill) {
        return PromiseResolve(createReadResult(this.#convert(desc), false));
      }
      if (this.queue.getEntry(this.position) === CLOSE_SENTINEL) {
        // The cursor faces the sentinel: no more data can ever arrive
        // for this descriptor.
        if (desc.bytesFilled > 0 && desc.bytesFilled % desc.elementSize !== 0) {
          // Fractional element fill — the remaining bytes can never
          // complete an element; error the stream immediately.
          const e = new TypeError(
            'Insufficient bytes to fill elements in the given view'
          );
          if (this.#errorStreamCallback !== undefined) {
            this.#errorStreamCallback(e, this.ownerDeref());
          }
          return PromiseReject(e);
        }
        // Element-aligned (possibly empty) fill: settle via the deferred
        // end-of-data settlement.
        this.#scheduleEndOfDataSettlement();
      }
    }
    this.#pendingPullIntos.push(desc);
    return desc.promise;
  }

  // Keep filling head descriptors from newly queued data while they can be
  // completed; then let the base class service default pending reads
  // (including sentinel handling) and refresh backpressure.
  override notify(): void {
    this.#processPullIntos(undefined);
  }

  // `committed` is respond()'s head, already removed from the list; it
  // settles ahead of the descriptors filled here (spec
  // RespondInReadableState steps 11-13).
  #processPullIntos(committed: PullIntoDescriptor | undefined): void {
    // Two-phase processing per spec
    // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue:
    // fill ALL ready descriptors first, THEN resolve them. This ensures
    // byobRequest is null by the time any resolve fires (which may trigger
    // user-observable code via Object.prototype.then interception).
    let filledPullIntos:
      Array<{ desc: PullIntoDescriptor; view: ArrayBufferView }> | undefined;
    if (committed !== undefined) {
      filledPullIntos = [{ desc: committed, view: this.#convert(committed) }];
    }
    let errored = false;
    while (this.#pendingPullIntos.length > 0) {
      const slot = this.queue.getEntry(this.position);
      // A prefix precedes queued data, never the close sentinel.
      if (slot === undefined && this.#prefix === undefined) break;
      if (slot === CLOSE_SENTINEL) {
        // End of DATA. Check for fractional-element fill: if any BYOB
        // descriptor has partially filled bytes that don't align to the
        // element size, the remaining bytes can never complete an element
        // — the stream must be errored with a TypeError (spec
        // ReadableByteStreamControllerClose step 4).
        if (this.#checkFractionalFillAtClose()) {
          errored = true;
          break;
        }
        // Synthetic descriptors for DEFAULT reads (autoAllocateChunkSize)
        // follow default-read close semantics: ReadableStreamClose drains
        // read requests with done, so they resolve { done: true } now.
        this.#resolveDefaultPullIntosAsDone();
        // TRUE BYOB reads settle via the deferred end-of-data settlement:
        // the source may still call respond(0) in this same turn
        // (RespondInClosedState reaches commitPullIntosOnClose() via the
        // controller and claims the spec's fold shape first); whatever is
        // left when the microtask runs settles with the C++-parity tail
        // shape while retaining its descriptor for a later closed-state
        // response. In multi-cursor mode (tee branches), no byobRequest
        // covers a branch's reads and respond(0) cannot reach them, so the
        // deferred settlement is what settles every branch read.
        this.#scheduleEndOfDataSettlement();
        break;
      }
      const head = this.#pendingPullIntos.peek() as PullIntoDescriptor;
      if (head.readerType === 'none') {
        // A released reader's head never takes new data. enqueue() and
        // respond() remove it before notifying; this is a backstop.
        this.#pendingPullIntos.shift();
        this.#moveToPrefix(head);
        continue;
      }
      const prefixView = this.#takePrefixForDefaultPullInto();
      if (prefixView !== undefined) {
        if (filledPullIntos === undefined) filledPullIntos = [];
        ArrayPrototypePush(filledPullIntos, { desc: head, view: prefixView });
        continue;
      }
      this.#fillFromQueue(head);
      if (head.bytesFilled < head.minimumFill) break; // need more data
      this.#pendingPullIntos.shift();
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
    if (errored) return;
    if (this.#prefix !== undefined && super.hasPendingRead) {
      this.fulfillFirstPendingRead(this.#takePrefix());
    }
    super.notify();
  }

  // Spec ReadableByteStreamControllerClose step 4: if any pending BYOB
  // descriptor has a fractional element fill (bytesFilled > 0 but not
  // element-aligned), the remaining bytes can never form a complete element
  // — error the stream with TypeError and reject all pending reads. Returns
  // true if the stream was errored (caller should bail out of notify).
  #checkFractionalFillAtClose(): boolean {
    const pending = this.#pendingPullIntos;
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      if (desc.bytesFilled > 0 && desc.bytesFilled % desc.elementSize !== 0) {
        const e = new TypeError(
          'Insufficient bytes to fill elements in the given view'
        );
        if (this.#errorStreamCallback !== undefined) {
          this.#errorStreamCallback(e, this.ownerDeref());
        }
        // errorAllReads is called by the callback's error path (the
        // controller's error() or readableStreamErrorBranch), not here.
        return true;
      }
    }
    return false;
  }

  // The deferred end-of-data settlement. Scheduled (once per turn) whenever
  // BYOB descriptors face the close sentinel — from notify() when close()
  // lands with reads parked, and from readBYOB() when a read submitted
  // against a closed-but-undrained queue fills below its minimum. The
  // one-microtask deferral lets a source that calls
  // respond(0)/respondWithNewView(empty) in the same turn as close()
  // commit first via commitPullIntosOnClose() and claim the spec's
  // RespondInClosedState fold shape ({ done: true, value: partial }, the
  // WPT read-min pinned behavior). Otherwise the read falls through to the
  // C++-parity tail shape, without invalidating the descriptor.
  //
  // CONSEQUENCE — the result shape depends on microtask timing. With a
  // partially filled read parked, `close(); respond(0)` in one turn yields
  // { done: true, value: partial }, while `close(); await x; respond(0)`
  // yields { done: false, value: partial } followed by { done: true,
  // value: empty } on the next read. Same source, one await apart,
  // different consumer-visible shape. Accepted: the spec alternative for a
  // source that never responds after close() is a read that pends forever.
  #scheduleEndOfDataSettlement(): void {
    if (this.#endOfDataSettlementScheduled) return;
    this.#endOfDataSettlementScheduled = true;
    PromisePrototypeThen(PromiseResolve(), () => {
      this.#endOfDataSettlementScheduled = false;
      this.#settlePullIntosAtEndOfData();
    });
  }

  // Settle every still-pending read with the tail shape: element-aligned
  // partial fills resolve { done: false, value: partial } (a subsequent
  // read observes the closed stream and resolves done with an empty view —
  // the C++ readAtLeast tail contract); unfilled descriptors resolve
  // { done: true, value: empty view }. Either way the reader gets its own
  // buffer back: it is transferred once (O(1), no copy — the point of
  // BYOB) so any view the source still holds over the old buffer
  // (byobRequest.view) detaches, as the spec's commit-time transfer would.
  // The descriptor stays listed, pointing at the transferred buffer, so a
  // later closed-state response remains legal; settledAtEndOfData tells
  // those paths not to transfer again. (A byobRequest minted after
  // settlement therefore aliases the delivered buffer's unfilled tail;
  // writing into it after close() is a source bug with no spec meaning.)
  // Bails when the cursor no longer faces the sentinel: error() dropped
  // the entries (getEntry returns undefined), and cancel()/respond(0)
  // paths emptied the descriptor list.
  #settlePullIntosAtEndOfData(): void {
    if (this.queue.getEntry(this.position) !== CLOSE_SENTINEL) return;
    const pending = this.#pendingPullIntos;
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      // 'none': released reader (or an auto-allocate read already
      // resolved by #resolveDefaultPullIntosAsDone, or a BYOB read settled
      // by an earlier invocation) — nothing to settle.
      if (desc.readerType === 'none') continue;
      if (desc.readerType === 'default') {
        // Defensive: auto-allocate default reads normally resolve in
        // #resolveDefaultPullIntosAsDone before this runs. Default-read
        // close semantics: done with value undefined, never a view.
        desc.readerType = 'none';
        desc.resolve(createReadResult(undefined, true));
      } else {
        // assert: desc.bytesFilled % desc.elementSize === 0 (fractional
        // fills errored the stream before settlement could be scheduled)
        desc.buffer = ArrayBufferPrototypeTransferToFixedLength(desc.buffer);
        const view = this.#convert(desc);
        desc.readerType = 'none';
        desc.settledAtEndOfData = true;
        if (desc.bytesFilled > 0) {
          desc.resolve(createReadResult(view, false));
        } else {
          desc.resolve(createReadResult(view, true));
        }
      }
    }
  }

  // Resolve synthetic default-read descriptors (autoAllocateChunkSize) as
  // done at end-of-stream; true BYOB reads are left for the deferred
  // end-of-data settlement (or a same-turn respond(0)). In practice the list
  // is homogeneous (single reader type at a time), so the filtering is
  // defensive.
  //
  // The descriptor is KEPT in the list (not shifted) so that a subsequent
  // respond(0) finds it and doesn't throw. commitPullIntosOnClose skips
  // already-resolved descriptors.
  #resolveDefaultPullIntosAsDone(): void {
    const pending = this.#pendingPullIntos;
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
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
    const head = this.#pendingPullIntos.peek();
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
      this.#pendingPullIntos.shift();
      if (head.bytesFilled > 0) {
        // Clone (not transfer) the filled portion into a new queue entry.
        // enqueue triggers notify() which fills subsequent descriptors.
        this.queue.enqueue({
          value: {
            buffer: cloneArrayBuffer(
              head.buffer,
              head.byteOffset,
              head.bytesFilled
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
    // Spec ReadableByteStreamControllerRespondInReadableState steps 7-13.
    this.#pendingPullIntos.shift();
    const remainderSize = head.bytesFilled % head.elementSize;
    if (remainderSize > 0) {
      // The remainder bytes live at the END of the filled region.
      const end = head.byteOffset + head.bytesFilled;
      // Queued without notifying: the head must settle before any read
      // the remainder fills.
      this.queue.enqueue(
        {
          value: {
            buffer: cloneArrayBuffer(
              head.buffer,
              end - remainderSize,
              remainderSize
            ),
            byteOffset: 0,
            byteLength: remainderSize,
          },
          size: remainderSize,
        },
        false
      );
      // Truncate bytesFilled to an element-aligned boundary.
      head.bytesFilled -= remainderSize;
    }
    // Fills later descriptors from the queue, then settles the head ahead
    // of them. The default-read/backpressure follow-ups are no-ops here.
    this.#processPullIntos(head);
  }

  // Commit all pending pull-into descriptors at end-of-stream: resolve with
  // the filled-so-far view (possibly zero-length — the buffer is handed
  // back) and done: true in a SINGLE result — the spec's
  // RespondInClosedState fold shape. Called by the controller from the
  // respond(0)-while-closed path ONLY; descriptors the source never
  // responds to settle through #settlePullIntosAtEndOfData instead, with
  // the split tail shape; their descriptors stay here so a later response
  // remains legal. A fractional-element fill never reaches this point:
  // controller.close() throws for it.
  commitPullIntosOnClose(): void {
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = new RingBuffer();
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      // Skip already-resolved descriptors (auto-allocate close and deferred
      // end-of-data paths set readerType to 'none').
      if (desc.readerType === 'none') continue;
      // assert: desc.bytesFilled % desc.elementSize === 0
      desc.resolve(createReadResult(this.#convert(desc), true));
    }
  }

  // Spec ReadableByteStreamControllerEnqueue step 9.3: if the head
  // descriptor is an auto-allocate (readerType 'default'), shift it out
  // so the controller can fulfill the read directly from the enqueued
  // chunk (bypassing the queue and the auto-allocate buffer).
  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined {
    const head = this.#pendingPullIntos.peek();
    if (head === undefined || head.readerType !== 'default') return undefined;
    this.#pendingPullIntos.shift();
    return head;
  }

  // Stream error: partial fills are lost.
  override errorAllReads(reason: unknown): void {
    this.#prefix = undefined;
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = new RingBuffer();
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      desc.reject(reason);
    }
    super.errorAllReads(reason);
  }

  // Stream cancel: pending BYOB reads resolve { done: true, value:
  // undefined } — partial data and buffers are dropped (spec,
  // WPT-verified).
  override resolveAllReadsAsDone(): void {
    this.#prefix = undefined;
    const pending = this.#pendingPullIntos;
    this.#pendingPullIntos = new RingBuffer();
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      desc.resolve(createReadResult(undefined, true));
    }
    super.resolveAllReadsAsDone();
  }

  // Reject pull-intos submitted by a specific reader (lock release).
  override cancelReadsForReader(reader: object, reason: unknown): void {
    // Spec ReleaseSteps: the reader's reads reject and pendingPullIntos
    // shrinks to its head, marked 'none'; respond()/enqueue() then move the
    // head's filled bytes ahead of new data. The byobRequest (over the head)
    // is NOT invalidated.
    const pending = this.#pendingPullIntos;
    for (let i = 0; i < pending.length; i++) {
      const desc = pending.get(i) as PullIntoDescriptor;
      if (desc.reader === reader) {
        desc.reject(reason);
      }
    }
    const head = pending.peek();
    if (head !== undefined) {
      head.readerType = 'none';
      const kept: RingBufferType<PullIntoDescriptor> = new RingBuffer();
      kept.push(head);
      this.#pendingPullIntos = kept;
    }
    super.cancelReadsForReader(reader, reason);
  }

  // The ONLY place result views are built. Construction count is in
  // ELEMENTS (bytesFilled / elementSize) — passing bytes happens to work
  // for Uint8Array and is wrong for every other view type. For DataView,
  // elementSize is 1 and its length parameter is in bytes, so the same
  // expression holds.
  #convert(
    desc: PullIntoDescriptor,
    buffer: ArrayBuffer = desc.buffer
  ): ArrayBufferView {
    // assert: desc.bytesFilled % desc.elementSize === 0
    // assert: desc.bytesFilled <= desc.byteLength
    return ReflectConstruct(desc.viewCtor, [
      buffer,
      desc.byteOffset,
      desc.bytesFilled / desc.elementSize,
    ]);
  }

  // Fill `desc` from the queue starting at (position, byteOffset). Mirrors
  // ReadableByteStreamControllerFillPullIntoDescriptorFromQueue:
  //   - if queued data reaches an element-aligned boundary >= minimumFill,
  //     copy only up to that aligned boundary (remainder bytes stay queued
  //     for the next read) — descriptor is then ready;
  //   - otherwise copy everything available (the descriptor may temporarily
  //     end mid-element) and stay pending.
  // Takes the prefix first. Stops at the CLOSE_SENTINEL. Advances
  // position/byteOffset for consumed bytes (which triggers GC + backpressure
  // refresh via setConsumed).
  #fillFromQueue(desc: PullIntoDescriptor): void {
    // Byte entries are sized by byteLength, so the running total is exactly
    // the bytes ahead of this cursor: the prefix, plus the queue up to the
    // sentinel, less the consumed part of the current entry.
    const available = this.remainingSize;
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

    const prefix = this.#prefix;
    if (prefix !== undefined) {
      const n = MathMin(prefix.byteLength, remaining);
      TypedArrayPrototypeSet(
        new Uint8Array(desc.buffer, desc.byteOffset + desc.bytesFilled, n),
        new Uint8Array(prefix.buffer, prefix.byteOffset, n)
      );
      desc.bytesFilled += n;
      remaining -= n;
      this.addToTotalSize(-n);
      if (n === prefix.byteLength) {
        this.#prefix = undefined;
      } else {
        prefix.byteOffset += n;
        prefix.byteLength -= n;
      }
    }

    let pos = this.position;
    let off = this.byteOffset;
    while (remaining > 0) {
      const slot = this.queue.getEntry(pos);
      // The running total guarantees a data entry here; running out means
      // the accounting is off, and stopping loses less than reading on.
      if (!isQueueEntry(slot)) break;
      const entry = slot.value;
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
  cloneArrayBuffer,
  createReadResult,
  StreamQueue,
  QueueCursor,
  ByteStreamCursor,
};
