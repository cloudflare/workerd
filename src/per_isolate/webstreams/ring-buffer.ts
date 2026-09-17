'use strict';

// RingBuffer: an O(1) FIFO with indexed access, backing every internal
// queue in the streams implementation (StreamQueue entries, pending reads
// and pull-intos, write requests, snapshot FIFOs). A plain array dequeued
// with shift()/splice(0, n) is O(n) once V8 can no longer left-trim the
// backing store, which made every backlog quadratic past ~20k entries.
//
// The backing store has a power-of-two capacity (index math is a mask),
// is allocated on the first push, and doubles when full. Every read is
// bounds-checked against #size and consumed slots are overwritten with
// undefined, so no access ever reads a hole: the buffer is immune to
// Array.prototype[N] pollution. Leaf module: requires nothing.

const { ArrayPrototypePush } = primordials;

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
    if (this.#size > this.#mask) this.#grow();
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
    return item;
  }

  // Remove and return the head item; undefined when empty.
  shift(): T | undefined {
    if (this.#size === 0) return undefined;
    const item = this.#backing[this.#head];
    this.#backing[this.#head] = undefined;
    this.#head = (this.#head + 1) & this.#mask;
    this.#size--;
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

  // Drop `count` items from the head. O(count). The backing store is kept.
  trimFront(count: number): void {
    if (count <= 0) return;
    if (count > this.#size) count = this.#size;
    for (let i = 0; i < count; i++) {
      this.#backing[(this.#head + i) & this.#mask] = undefined;
    }
    this.#size -= count;
    this.#head = this.#size === 0 ? 0 : (this.#head + count) & this.#mask;
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

  // Double the capacity, linearizing the items from the head. The new
  // store is filled by push: new Array(n) above ~32k elements would start
  // in V8's dictionary mode, whereas a pushed array stays fast and packed.
  #grow(): void {
    const backing = this.#backing;
    const head = this.#head;
    const size = this.#size;
    const mask = this.#mask;
    const capacity = mask + 1 === 0 ? kInitialCapacity : (mask + 1) * 2;
    const grown: (T | undefined)[] = [];
    for (let i = 0; i < size; i++) {
      ArrayPrototypePush(grown, backing[(head + i) & mask]);
    }
    for (let i = size; i < capacity; i++) {
      ArrayPrototypePush(grown, undefined);
    }
    this.#backing = grown;
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
