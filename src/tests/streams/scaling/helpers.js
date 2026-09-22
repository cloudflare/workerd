// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The suite's one assertion: a shape's cost grows linearly with the
// number of entries it queues. `run(n)` is timed at `small` entries and
// at `small × factor`; the large run may take at most `slack` times the
// linear `factor` multiple of the small run. A run is first warmed up at
// a fraction of `small`, so the small run is not JIT-cold. Both
// implementations run the same shapes, so the ratio, unlike an absolute
// bound, does not depend on the machine.

import { ok } from 'node:assert';

// A small run below this is timer noise; treat it as this long.
const FLOOR_MS = 25;

async function timeAt(run, n) {
  const start = performance.now();
  await run(n);
  return performance.now() - start;
}

export async function assertLinearScaling(run, small, factor = 8, slack = 4) {
  await run(small >> 3);
  const smallMs = await timeAt(run, small);
  const large = small * factor;
  const largeMs = await timeAt(run, large);
  const limit = Math.max(smallMs, FLOOR_MS) * factor * slack;
  ok(
    largeMs <= limit,
    `${large} entries took ${largeMs.toFixed(0)} ms, over ${limit.toFixed(0)} ms ` +
      `(${small} entries took ${smallMs.toFixed(0)} ms)`
  );
}
