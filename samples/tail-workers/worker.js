// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    console.log('log from worker a');
    return new Response(
      'response from worker a + \n' + (await env.log.respond('worker b'))
    );
  },
};
