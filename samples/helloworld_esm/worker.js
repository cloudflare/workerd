// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

throw new AggregateError([new Error('boom')], 'message', { cause: new Error('cause') });

export default {
  async fetch(req, env) {
    return new Response("Hello World\n");
  }
};
