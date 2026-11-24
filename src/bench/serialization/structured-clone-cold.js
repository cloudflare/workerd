// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone() in a new isolate. The top-level calls are the first in
// their isolate, before V8 has compiled or optimized any of the code they
// run; the startup group's startup:worker case times them, in a new isolate
// for every sample, and its startup:script case times the per-isolate
// bootstrap that installs the function. The module does nothing else at top
// level, so those two cases differ between implementations by those costs
// alone.

const SMALL = { a: 1, b: 'two', c: [3] };

structuredClone(SMALL);
const buffer = new ArrayBuffer(8);
structuredClone(buffer, { transfer: [buffer] });

export default {
  bench(b) {
    // The same call once warm, for comparison.
    b.run('small object', () => b.blackBox(structuredClone(SMALL)));
  },
};
