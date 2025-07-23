// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  async tail(events, env, ctx) {
    await env.TAIL_SERVICE.tail(events);
  },
};

export const test = {
  async test() {
    await scheduler.wait(50);
  },
};
