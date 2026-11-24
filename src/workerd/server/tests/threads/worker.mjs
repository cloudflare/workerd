// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The identity of this isolate: global state, so each copy of the Worker has its own.
let isolate;

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

// How many requests of this isolate wait for the end of their bodies.
let waiting = 0;

export default {
  async fetch(request, env) {
    // A request for another isolate is refused before its body is read, so that the client can
    // send it again until the isolate it names serves it.
    const wanted = request.headers.get('x-thread');
    if (wanted !== null && wanted !== isolateId()) {
      return Response.json({ front: isolateId() }, { status: 409 });
    }
    const url = new URL(request.url);
    if (url.pathname === '/waiting') {
      return Response.json({ front: isolateId(), waiting });
    }
    if (url.pathname === '/spin') {
      // Keeps this isolate's thread from serving anything else for a while.
      const until = Date.now() + Number(url.searchParams.get('ms'));
      while (Date.now() < until) {}
    }
    // The response waits for the end of the request body, which the client decides.
    waiting++;
    try {
      await request.arrayBuffer();
    } finally {
      waiting--;
    }
    const ids = { front: isolateId() };
    if (env.BACK !== undefined) {
      const back = await env.BACK.fetch('http://back/');
      ids.back = (await back.json()).front;
    }
    return Response.json(ids);
  },
};
