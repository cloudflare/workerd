// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    const cached = await env.CACHE.read("hello", async (key) => {
      return {
        value: 'World',
        expiration: Date.now() + 10000,
      };
    });

    return new Response(`Hello ${cached}\n`);
  }
};
