// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

let resposeMap = new Map();

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(args) {
    // Invalid log statement, causes worker to not return a valid handler
    console.log(args.map((t) => t.logs));
    return (args) => {
      console.log(args);
    };
  },
};

export const test = {
  async test() {
    await scheduler.wait(100);
    // Tests for a bug where we tried to report an outcome event to a stream after setting up the
    // stream handler with the onset event failed – with an invalid tail handler, we should not
    // report any further events.
    // TODO: How to test this better? What we want to see here is there not being an
    // "Expected only a single onset event" error.
    assert.ok(resposeMap.size == 0);
  },
};
