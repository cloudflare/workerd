// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `scheduler` global across a startup snapshot. A case Worker's top level
// (startup-snapshot-scheduler-case-test.js) keeps the object and its wait(), or leaves `scheduler`
// unread until a request, and at request time reports whether the object is whole and waits. The
// object is the C++ Scheduler, or under the TS_SCHEDULER autogate (in the @all-autogates variant)
// one that the per-isolate bootstrap makes. This runner asserts the reports. Under the
// startup-snapshot autogate every service here goes through the zygote; without it they are plain
// Workers and the same assertions hold with snapshotUsed false.

import { strictEqual } from 'node:assert';
import unsafe from 'workerd:unsafe';

export default {
  async test(ctrl, env) {
    // The runner went through the same pipeline as the cases: whether it was started from a
    // snapshot says whether the pipeline is on in this variant.
    const pipelineOn = unsafe.startedFromStartupSnapshot();
    const run = async (name) => {
      const response = await env[name].fetch('http://case/');
      const r = await response.json();
      strictEqual(r.mode, name);
      strictEqual(r.snapshotUsed, pipelineOn, `${name}: snapshotUsed`);
      for (const [check, value] of Object.entries(r.checks)) {
        strictEqual(value, true, `${name}: ${check}`);
      }
    };
    await run('kept');
    await run('unread');
  },
};
