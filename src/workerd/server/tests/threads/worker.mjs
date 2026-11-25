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
      return Response.json({ front: isolateId() }, { status: 409 });
    }
    if (attempt !== null) attempts.add(attempt);
    const url = new URL(request.url);
    if (url.pathname === '/open') {
      // The head goes out at once, and the body once the request body has ended.
      const front = isolateId();
      const body = new ReadableStream({
        async pull(controller) {
          await request.arrayBuffer();
          controller.enqueue(
            new TextEncoder().encode(JSON.stringify({ front }))
          );
          controller.close();
        },
      });
      return new Response(body, { headers: { 'x-front': front } });
    }
    if (url.pathname === '/hold') {
      // The head goes out at once and the body ends after `ms` milliseconds: until then the
      // request is in flight on this thread.
      const ms = Number(url.searchParams.get('ms'));
      const body = new ReadableStream(
        {
          async pull(controller) {
            await scheduler.wait(ms);
            controller.close();
          },
        },
        { highWaterMark: 0 }
      );
      return new Response(body, { headers: { 'x-front': isolateId() } });
    }
    if (url.pathname === '/spin') {
      // Keeps this isolate's thread from serving anything else for a while.
      const until = Date.now() + Number(url.searchParams.get('ms'));
      while (Date.now() < until) {}
    }
    // The response waits for the end of the request body, which the client decides.
    await request.arrayBuffer();
    const ids = { front: isolateId() };
    if (env.BACK !== undefined) {
      const back = await env.BACK.fetch('http://back/');
      ids.back = (await back.json()).front;
    }
    return Response.json(ids);
  },
};
