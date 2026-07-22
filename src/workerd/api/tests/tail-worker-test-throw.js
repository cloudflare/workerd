// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Throw an error when receiving a non-onset event to confirm that exceptions in tailStream()
    // are reproduced in the trace
    return (event) => {
      throw new Error('boom');
    };
  },
};
