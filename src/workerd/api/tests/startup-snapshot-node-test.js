// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Node.js API objects across a startup snapshot. A case Worker's top level
// (startup-snapshot-node-case-test.js) creates an object whose native side has to travel through
// the snapshot, the way bundled Node.js code commonly does at module scope, and at request time
// reports whether it still works. This runner asserts the reports. It imports none of the
// modules under test, so that its own snapshot does not depend on them. Under the
// startup-snapshot autogate every service here goes through the zygote; without it they are
// plain Workers and the same assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// See startup-snapshot-node-case-test.js.
const snapshotUsed = typeof env.SELF === 'undefined';

export default {
  async test(ctrl, env) {
    // The runner went through the same pipeline as the cases: whether it was started from a
    // snapshot says whether the pipeline is on in this variant.
    const pipelineOn = snapshotUsed;
    const run = async (name, expectedSnapshotUsed) => {
      const response = await env[name].fetch('http://case/');
      const r = await response.json();
      strictEqual(r.mode, name);
      strictEqual(
        r.snapshotUsed,
        expectedSnapshotUsed,
        `${name}: snapshotUsed`
      );
      for (const [check, value] of Object.entries(r.checks)) {
        strictEqual(value, true, `${name}: ${check}`);
      }
    };
    for (const name of ['als', 'diagnostics-channel']) {
      await run(name, pipelineOn);
    }
    await run('als-default', false);
  },
};
