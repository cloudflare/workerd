// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {

    const headers = new Headers();
    headers.set('content-type', 'application/x-www-form-urlencoded');
    const req2 = new Request('http://example.org', {
      method: 'POST',
      headers,
      body: [
        'field0=part0',
        'field1=part1',
        'field0=part2',
        'field1=part3',
      ].join('&')
    });

    const fd = await req2.formData();
    for (const [k,v] of fd) console.log(k,v);

    return new Response("Hello World\n");
  }
};
