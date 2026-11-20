// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `navigator` global across a startup snapshot. A case Worker's top level
// (startup-snapshot-navigator-case-test.js) either keeps the navigator and what it reads from
// it, or leaves `navigator` unread until a request, and at request time reports whether it is
// whole. The navigator is the C++ Navigator, or under the TS_NAVIGATOR autogate (in the
// @all-autogates variant) an object of the per-isolate bootstrap over a C++ helper. This runner
// asserts the reports. Under the startup-snapshot autogate every service here goes through the
// zygote; without it they are plain Workers and the same assertions hold with snapshotUsed
// false.

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
