// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The identity of this isolate: global state, so each copy of the Worker has its own.
let isolate;

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

// The `x-attempt` of every request this isolate acted on.
const attempts = new Set();

// The cache a request names: `caches.default`, or with `?name=` the one `caches.open()` opens.
async function cacheOf(url) {
  const name = url.searchParams.get('name');
  return name === null ? caches.default : await caches.open(name);
}

// A JSON response that carries the identity of the isolate that served it, as every response
// this Worker serves does.
function json(value) {
  return Response.json(value, { headers: { 'x-front': isolateId() } });
}

// The URL an entry of the caches is stored under.
function cacheKey(url) {
  return `http://cache.test/${url.searchParams.get('key')}`;
}

export default {
  async fetch(request, env) {
    // A request for another isolate is refused before its body is read, so that the client can
    // send it again until the isolate it names serves it. The refusal is held for a while: the
    // front gives the next request to a sleeping thread with the fewest requests in flight, so
    // the client's other attempts at once go to other threads.
    //
    // The attempts of one request carry one `x-attempt`, and the isolate named acts on the
    // first it gets only: it refuses the others too.
    const wanted = request.headers.get('x-thread');
    const attempt = request.headers.get('x-attempt');
    if (
      wanted !== null &&
      (wanted !== isolateId() || (attempt !== null && attempts.has(attempt)))
    ) {
      await scheduler.wait(100);
      return Response.json(
        { front: isolateId() },
        { status: 409, headers: { 'x-refused': isolateId() } }
      );
    }
    if (attempt !== null) attempts.add(attempt);
    const url = new URL(request.url);
    const front = isolateId();

    if (url.pathname === '/where') {
      // Answers after `ms` milliseconds, so that the request is in flight on this thread
      // meanwhile.
      await scheduler.wait(Number(url.searchParams.get('ms') ?? 0));
      return json({ front });
    }

    if (url.pathname.startsWith('/bucket/')) {
      // The request's method and body, to the bucket service for the key after `/bucket/`; its
      // response as the service gave it.
      const key = url.pathname.slice('/bucket/'.length);
      const response = await env.BUCKET.fetch(`http://bucket/${key}`, {
        method: request.method,
        body: request.body,
      });
      const answer = new Headers(response.headers);
      answer.set('x-front', front);
      return new Response(response.body, {
        status: response.status,
        headers: answer,
      });
    }

    if (url.pathname === '/cache/put') {
      const cache = await cacheOf(url);
      await cache.put(
        cacheKey(url),
        new Response(request.body, {
          headers: { 'Cache-Control': 'max-age=3600' },
        })
      );
      return json({ front });
    }
    if (url.pathname === '/cache/match') {
      const cache = await cacheOf(url);
      const response = await cache.match(cacheKey(url));
      if (response === undefined) return json({ front, hit: false });
      return json({
        front,
        hit: true,
        status: response.status,
        cacheStatus: response.headers.get('cf-cache-status'),
        body: await response.text(),
      });
    }
    if (url.pathname === '/cache/delete') {
      const cache = await cacheOf(url);
      return json({ front, deleted: await cache.delete(cacheKey(url)) });
    }

    return new Response('not found', { status: 404 });
  },
};
