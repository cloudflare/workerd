// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Module identity and import.meta across a startup snapshot with the new module registry. The
// case Worker (startup-snapshot-new-module-registry-case-test.js) imports a bundle ES module and a
// JSON module at the top level, then at request time imports them again, imports a module its
// top level never loaded, and reads import.meta. This runner asserts the report. Under the
// startup-snapshot autogate both services go through the zygote; without it they are plain
// Workers and the same assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// See startup-snapshot-new-module-registry-case-test.js.
const snapshotUsed = typeof env.SELF === 'undefined';

export default {
  async test(ctrl, env) {
    // The runner went through the same pipeline as the case: whether it was started from a
    // snapshot says whether the pipeline is on in this variant.
    const response = await env.case.fetch('http://case/');
    const r = await response.json();
    strictEqual(r.snapshotUsed, snapshotUsed, 'case: snapshotUsed');
    for (const [check, value] of Object.entries(r.checks)) {
      strictEqual(value, true, `case: ${check}`);
    }
  },
};
