// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    let resp = await fetch("https://example.com");

    // GC TEST
    let cleanup0_call_count = 0;
    let cleanup1_call_count = 0;

    let cleanup0 = function (holdings) {
      ++cleanup0_call_count;
    }

    let cleanup1 = function (holdings) {
      ++cleanup1_call_count;
    }

    let fg0 = new FinalizationRegistry(cleanup0);
    let fg1 = new FinalizationRegistry(cleanup1);

    // Register 1 weak reference for each FinalizationRegistry and kill the
    // objects they point to.
    (function () {
      // The objects need to be inside a closure so that we can reliably kill
      // them.
      let objects = [];
      objects[0] = {};
      objects[1] = {};
      fg0.register(objects[0], "holdings0-0");
      fg1.register(objects[1], "holdings1-0");
      // Drop the references to the objects.
      objects = [];
    })();

    // Schedule a GC, which will schedule both fg0 and fg1 for cleanup.
    // Here and below, we need to invoke GC asynchronously and wait for it to
    // finish, so that it doesn't need to scan the stack. Otherwise, the objects
    // may not be reclaimed because of conservative stack scanning and the test
    // may not work as intended.
    let task_1_gc = (async function () {
      gc({ type: 'major', execution: 'async' });

      // Before the cleanup task has a chance to run, do the same thing again, so
      // both FinalizationRegistries are (again) scheduled for cleanup. This has to
      // be a IIFE function (so that we can reliably kill the objects) so we cannot
      // use the same function as before.
      (function () {
        let objects = [];
        objects[0] = {};
        objects[1] = {};
        fg0.register(objects[0], "holdings0-1");
        fg1.register(objects[1], "holdings1-1");
        objects = [];
      })();
    })();

    // Schedule a second GC for execution after that, which will again schedule
    // both fg0 and fg1 for cleanup.
    let task_2_gc = (async function () {
      gc({ type: 'major', execution: 'async' });

      // Check that no cleanup task has had the chance to run yet.
      console.log("Expected 0:" + cleanup0_call_count);
      console.log("Expected 0:" + cleanup1_call_count);
    })();

    // Wait for the two GCs to be executed.
    await task_1_gc;
    await task_2_gc;

    // Wait two ticks, so that the finalization registry cleanup has an
    // opportunity to both run and re-post itself.
    await new Promise(resolve=>setTimeout(resolve, 1000));
    await new Promise(resolve=>setTimeout(resolve, 1000));

    console.log("Expected 2:" + cleanup0_call_count);
    console.log("Expected 2:" + cleanup1_call_count);
    // GC TEST

    return resp;
  }
};
