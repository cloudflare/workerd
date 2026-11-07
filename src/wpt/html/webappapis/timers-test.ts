// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'clearinterval-from-callback.any.js': {},
  'cleartimeout-clearinterval.any.js': {},
  'evil-spec-example.any.js': {
    comment:
      'Passes a non-function handler, which WebIDL stringifies and evaluates as code. workerd does not support string handlers because it disallows eval; setTimeout throws a TypeError at the top level.',
    omittedTests: true,
  },
  'missing-timeout-setinterval.any.js': {},
  'negative-setinterval.any.js': {},
  'negative-settimeout.any.js': {},
  'setinterval-settimeout-clamping.any.js': {},
  'type-long-setinterval.any.js': {
    comment:
      'Hangs: workerd takes the setInterval delay as a double instead of converting it to a WebIDL long, so a delay of 2 ** 32 waits about 49 days instead of wrapping to 0. done() is never called, so the single test never completes.',
    disabledTests: true,
  },
  'type-long-settimeout.any.js': {
    comment:
      'Hangs: workerd takes the setTimeout delay as a double instead of converting it to a WebIDL long, so a delay of 2 ** 32 waits about 49 days instead of wrapping to 0. done() is never called, so the single test never completes.',
    disabledTests: true,
  },
  'zero-settimeout.any.js': {
    // The ordering assertion requires registration within one millisecond. Instrumentation or
    // preemption can cross that boundary, letting the 1ms timer expire before 0ms is registered.
    // Retry only registration across a clock tick; retain the upstream ordering assertion.
    replace(code) {
      const registration = `  setTimeout(t.step_func(() => append(2)), 2);
  setTimeout(t.step_func(() => append(1)), 1);
  setTimeout(t.step_func(() => append(0)), 0);`;
      if (!code.includes(registration)) {
        throw new Error('The zero-settimeout registration block has changed');
      }
      return code.replace(
        registration,
        `  for (let attempt = 0; attempt < 100; attempt++) {
    const before = Date.now();
    const timers = [
      setTimeout(t.step_func(() => append(2)), 2),
      setTimeout(t.step_func(() => append(1)), 1),
      setTimeout(t.step_func(() => append(0)), 0),
    ];
    if (Date.now() === before) break;
    for (const timer of timers) clearTimeout(timer);
    assert_less_than(attempt, 99, 'Could not register timers within one millisecond');
  }`
      );
    },
  },
} satisfies TestRunnerConfig;
