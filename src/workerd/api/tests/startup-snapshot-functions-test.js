// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Functions implemented in C++ across a startup snapshot. A case Worker's top level
// (startup-snapshot-functions-case-test.js) keeps a function that C++ created, as bundled Node.js
// code commonly does with createRequire(import.meta.url), and calls it at request time. A function
// whose native state cannot be copied makes the zygote reject the snapshot. This
// runner asserts the reports. Under the startup-snapshot autogate every service here goes through
// the zygote; without it they are plain Workers and the same assertions hold with snapshotUsed
// false.

import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

// See startup-snapshot-functions-case-test.js.
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
    await run('create-require', pipelineOn);
    // Cases the zygote rejects: always plain Workers.
    await run('bind', false);
  },
};
