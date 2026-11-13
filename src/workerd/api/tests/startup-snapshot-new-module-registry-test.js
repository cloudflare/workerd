// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Module identity and import.meta across a startup snapshot with the new module registry. The
// case Worker (startup-snapshot-new-module-registry-case-test.js) imports a bundle ES module, a
// JSON module, a module under a query and a fragment, and a builtin at the top level, then at
// request time imports them again, imports a module its top level never loaded, and reads
// import.meta. This runner asserts the report. Under the
// startup-snapshot autogate both services go through the zygote; without it they are plain
// Workers and the same assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export default {
  async test(ctrl, env) {
    // The runner went through the same pipeline as the case: whether it was started from a
    // snapshot says whether the pipeline is on in this variant.
    const pipelineOn = unsafe.startedFromStartupSnapshot();
    const response = await env.case.fetch('http://case/');
    const r = await response.json();
    strictEqual(r.snapshotUsed, pipelineOn, 'case: snapshotUsed');
    for (const [check, value] of Object.entries(r.checks)) {
      strictEqual(value, true, `case: ${check}`);
    }
  },
};
