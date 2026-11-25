// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';

const ARRAY_LENGTH = 1024;
// Arrays of ARRAY_LENGTH elements that fit in a megabyte.
const ARRAYS_PER_MB = 256;
// One array in KEEP_ONE_IN outlives the scavenges. If most of them did, V8 would start to
// allocate them in the old generation, which checks the allocation limit.
const KEEP_ONE_IN = 8;
// The megabyte of arrays in the ring outlives eight scavenges, and two promote an array.
const ring = new Array(ARRAYS_PER_MB).fill(null);
let next = 0;
let tracked;

export default class extends WorkerEntrypoint {
  // Leaves `megabytes` MB of garbage in the old generation, without allocating there: the
  // scavenger promotes the arrays that later become garbage. Tracks one more promoted object
  // that is garbage when this returns, which only a full GC can collect.
  makeOldGarbage(megabytes) {
    // A WeakRef keeps its target alive until the end of the job that created it. That is long
    // enough for the scavenges below to promote the target.
    tracked = new WeakRef({});
    const count = Math.max(megabytes, 1) * ARRAYS_PER_MB * KEEP_ONE_IN;
    for (let i = 0; i < count; i++) {
      const array = new Array(ARRAY_LENGTH).fill(i);
      if (megabytes > 0 && i % KEEP_ONE_IN === 0) {
        ring[next++ % ring.length] = array;
      }
    }
    ring.fill(null);
  }

  // Whether a full GC has run since makeOldGarbage() returned.
  isTrackedCollected() {
    return tracked.deref() === undefined;
  }

  // A request that stays in flight until `release()` returns.
  async hold(release) {
    await release();
  }
}
