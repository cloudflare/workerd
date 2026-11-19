// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from 'cloudflare:workers';

// The identity of this isolate and what it counted: global state, which every loop that serves
// this copy of the Worker shares.
let isolate;
let count = 0;
// The keys whose `waitUntil()` task ended.
const done = new Map();

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

// The paragraphs of the document `/rewrite` streams.
const PARAGRAPHS = 200;

// The document `/rewrite` streams, in small chunks with waits between some of them.
function htmlStream() {
  let html = '<!doctype html><html><head><title>t</title></head><body>\n';
  for (let n = 0; n < PARAGRAPHS; n++) {
    html += `<p data-n="${n}">paragraph ${n}</p>\n`;
  }
  html += '</body></html>\n';
  const bytes = new TextEncoder().encode(html);
  const CHUNK = 37;
  let offset = 0;
  let chunks = 0;
  return new ReadableStream({
    async pull(controller) {
      if (offset >= bytes.length) {
        controller.close();
        return;
      }
      if (++chunks % 5 === 0) await scheduler.wait(1);
      controller.enqueue(bytes.slice(offset, offset + CHUNK));
      offset += CHUNK;
    },
  });
}

export class Counter extends DurableObject {
  #count = 0;

  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/do/count') {
      return Response.json({ count: ++this.#count, object: isolateId() });
    }
    if (url.pathname === '/do/ws') {
      const [client, server] = Object.values(new WebSocketPair());
      server.accept();
      server.addEventListener('message', (event) => {
        server.send(`echo: ${event.data}`);
      });
      server.addEventListener('close', (event) => server.close(event.code));
      return new Response(null, { status: 101, webSocket: client });
    }
    return new Response('not found', { status: 404 });
  }
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname.startsWith('/do/')) {
      const object = env.NS.get(env.NS.idFromName('x'));
      if (url.pathname === '/do/count') {
        // The object answers without the body: the end of it, which the client decides, comes
        // first.
        await request.arrayBuffer();
        return await object.fetch('http://object/do/count');
      }
      return await object.fetch(request);
    }
    switch (url.pathname) {
      case '/count':
        // The response waits for the end of the request body, which the client decides.
        await request.arrayBuffer();
        return Response.json({ isolate: isolateId(), count: ++count });
      case '/back': {
        await request.arrayBuffer();
        const back = await env.BACK.fetch('http://back/count', {
          method: 'POST',
        });
        return Response.json({ front: isolateId(), back: await back.json() });
      }
      case '/resume':
        for (let i = 0; i < 20; i++) await scheduler.wait(1);
        return new Response('resumed');
      case '/timers': {
        const start = Date.now();
        await scheduler.wait(50);
        const wait = Date.now() - start;
        const timeoutStart = Date.now();
        await new Promise((resolve) => setTimeout(resolve, 20));
        const timeout = Date.now() - timeoutStart;
        return Response.json({ wait, timeout });
      }
      case '/wait-until': {
        const key = url.searchParams.get('key');
        ctx.waitUntil(
          scheduler.wait(100).then(() => done.set(key, isolateId()))
        );
        return new Response('responded');
      }
      case '/done':
        return Response.json({ done: done.has(url.searchParams.get('key')) });
      case '/rewrite':
        return new HTMLRewriter()
          .on('p', {
            async element(element) {
              await scheduler.wait(1);
              const n = Number(element.getAttribute('data-n'));
              element.setInnerContent(`rewritten ${n * 2}`);
            },
          })
          .transform(
            new Response(htmlStream(), {
              headers: { 'content-type': 'text/html' },
            })
          );
    }
    return new Response('not found', { status: 404 });
  },
};
