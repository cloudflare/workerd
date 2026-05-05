// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Return a handler object that does not include any handler functions for individual tail
    // events – serves as a regression test for a bug where a handler that did not have handler
    // functions for any events in the final tail stream RPC call caused stream cancellation
    // warnings.
    return {};
  },
};
