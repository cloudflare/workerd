'use strict';

// RingBuffer: an O(1) FIFO with indexed access, backing every internal
// queue in the streams implementation (StreamQueue entries, pending reads
// and pull-intos, write requests, snapshot FIFOs). A plain array dequeued
// with shift()/splice(0, n) is O(n) once V8 can no longer left-trim the
// backing store, which made every backlog quadratic past ~20k entries.
//
// The backing store has a power-of-two capacity (index math is a mask),
// is allocated on the first push, doubles when full, and shrinks to fit
// once occupancy falls to a quarter (see #shrinkIfSparse). Every read is
// bounds-checked against #size and consumed slots are overwritten with
// undefined, so no access ever reads a hole: the buffer is immune to
// Array.prototype[N] pollution. Leaf module: requires nothing.

const kInitialCapacity = 16;

class RingBuffer<T> {
  #backing: (T | undefined)[] = [];
  #head: number = 0;
  #size: number = 0;
  #mask: number = -1; // capacity - 1; -1 until the first push allocates

  get length(): number {
    return this.#size;
  }

  // Append at the tail. Amortized O(1).
  push(item: T): void {
    if (this.#size > this.#mask) {
      const capacity = this.#mask + 1;
      this.#resize(capacity === 0 ? kInitialCapacity : capacity * 2);
    }
    this.#backing[(this.#head + this.#size) & this.#mask] = item;
    this.#size++;
  }

  // Remove and return the tail item; undefined when empty.
  pop(): T | undefined {
    if (this.#size === 0) return undefined;
    const index = (this.#head + this.#size - 1) & this.#mask;
    const item = this.#backing[index];
    this.#backing[index] = undefined;
    this.#size--;
    this.#shrinkIfSparse();
    return item;
  }

  // Remove and return the head item; undefined when empty.
  shift(): T | undefined {
    if (this.#size === 0) return undefined;
    const item = this.#backing[this.#head];
    this.#backing[this.#head] = undefined;
    this.#head = (this.#head + 1) & this.#mask;
    this.#size--;
    this.#shrinkIfSparse();
    return item;
  }

  // The head item without removing it; undefined when empty.
  peek(): T | undefined {
    return this.#size === 0 ? undefined : this.#backing[this.#head];
  }

  // The item at `index` from the head (0 = head); undefined when out of
  // range.
  get(index: number): T | undefined {
    if (index < 0 || index >= this.#size) return undefined;
    return this.#backing[(this.#head + index) & this.#mask];
  }

  // Drop `count` items from the head. O(count).
  trimFront(count: number): void {
    if (count <= 0) return;
    if (count > this.#size) count = this.#size;
    const size = this.#size - count;
    if (this.#isSparse(size)) {
      // The store is about to be replaced or released; the dropped slots
      // go with it, so there is no need to null them out.
      this.#size = size;
      this.#head = (this.#head + count) & this.#mask;
      this.#shrinkIfSparse();
      return;
    }
    for (let i = 0; i < count; i++) {
      this.#backing[(this.#head + i) & this.#mask] = undefined;
    }
    this.#size = size;
    this.#head = size === 0 ? 0 : (this.#head + count) & this.#mask;
  }

  // Drop everything, including the backing store. O(1); for terminal
  // paths (error, abort, cancel), where releasing the capacity matters
  // more than reusing it.
  clear(): void {
    this.#backing = [];
    this.#head = 0;
    this.#size = 0;
    this.#mask = -1;
  }

  // Whether a store above the initial capacity would be at most a quarter
  // full at `size` items.
  #isSparse(size: number): boolean {
    const capacity = this.#mask + 1;
    return capacity > kInitialCapacity && size <= capacity >> 2;
  }

  // Shrink a sparse store to the smallest power of two holding twice the
  // occupancy, or release it when empty. Growth leaves a store half full
  // and shrinking leaves it between a quarter and half full, so the next
  // resize in either direction is at least a quarter of the capacity's
  // worth of operations away: each resize is paid for by the operations
  // since the previous one (amortized O(1)), and a buffer oscillating
  // across a threshold cannot resize on every operation. Stores at the
  // initial capacity are never shrunk, so small steady-state buffers keep
  // their slots.
  #shrinkIfSparse(): void {
    if (!this.#isSparse(this.#size)) return;
    if (this.#size === 0) {
      this.clear();
      return;
    }
    let capacity = kInitialCapacity;
    while (capacity < this.#size * 2) capacity *= 2;
    this.#resize(capacity);
  }

  // Replace the store with one of `capacity` slots holding the items from
  // index 0.
  #resize(capacity: number): void {
    const backing = this.#backing;
    const head = this.#head;
    const size = this.#size;
    const mask = this.#mask;
    const resized: (T | undefined)[] = [];
    resized.length = capacity;
    for (let i = 0; i < size; i++) {
      resized[i] = backing[(head + i) & mask];
    }
    this.#backing = resized;
    this.#head = 0;
    this.#mask = capacity - 1;
  }
}

// The constructor's type, for the require() sites: the loader erases the
// class's own type, and a typed constructor keeps `new RingBuffer<T>()`
// checked at each site.
export type RingBufferConstructor = new <T>() => RingBuffer<T>;

export type { RingBuffer };

module.exports = { RingBuffer };
