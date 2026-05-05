// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Return an invalid handler
    return 42;
  },
};

export const test = {
  async test() {
    await scheduler.wait(50);
    // Tests for a bug where we tried to report an outcome event to a stream after setting up the
    // stream handler with the onset event failed – with an invalid tail handler, we should not be
    // failing the entire test but only return an error for the affected worker.
  },
};
