// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {

    const r = new Request('https://example.com/', {
      headers: {
        'x-hello': 'world',
      }
    });

    const r2 = new Request(new Proxy(r, {
      get(...args) {
        return Reflect.get(...args);
      }
    }));

    console.log(r2.url);
    console.log(r2.headers);

    return new Response("Hello World\n");
  }
};
