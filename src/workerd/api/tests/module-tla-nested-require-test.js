// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression tests for a V8 fatal CHECK ("status() >= kEvaluatingAsync") in the legacy module
// registry. See the .wd-test for the module graph. These are end-to-end rather than jsg unit
// tests because the legacy registry has no C++ harness and the failure mode is a process abort.
import { strictEqual, rejects } from 'node:assert';

export const nestedRequireDoesNotCrashSiblingTlaModule = {
  async test() {
    // Before the fix this aborted the process.
    const entry = await import('entry');
    strictEqual(entry.default, 1);
  },
};

export const nestedDynamicImportDoesNotCrashSiblingTlaModule = {
  async test() {
    // Before the fix this aborted the process: the nested dynamic import drained the
    // microtask queue while dyn-entry was still kEvaluating.
    const mod = await import('dyn-entry');
    strictEqual(mod.default, 1);
  },
};

export const dynamicImportStillSettlesTopLevelAwait = {
  async test() {
    // The depth-0 drain is what settles this module's top-level await.
    const mod = await import('dynamic-tla');
    strictEqual(mod.value, 42);

    // Repeat to confirm the depth counter was restored.
    const again = await import('dynamic-tla');
    strictEqual(again.value, 42);
  },
};

export const unsettledTopLevelAwaitStillThrows = {
  async test() {
    await rejects(import('never-settles'), {
      message: 'Top-level await in module is unsettled.',
    });
  },
};

export const nestedRequireOfUnsettledModuleThrows = {
  async test() {
    // require() is synchronous, so unlike a nested dynamic import this cannot be deferred to
    // the depth-0 drain. It must throw rather than expose a half-evaluated namespace. The
    // message differs by compat flag: without disable_top_level_await_in_require the
    // evaluation is attempted and reported unsettled, with it the async graph is rejected up
    // front as "not permitted".
    await rejects(import('req-entry'), /Top-level await in module/);
  },
};

export const throwingEvaluationDoesNotLeakEvaluationDepth = {
  async test() {
    // If the depth leaked on the exception path, isEvaluatingModule() would stay true and
    // every subsequent top-level await would be refused instead of drained.
    await rejects(import('boom'), { message: 'boom' });

    const mod = await import('dynamic-tla');
    strictEqual(mod.value, 42);
  },
};
