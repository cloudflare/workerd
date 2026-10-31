// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The event streams that connected EventSources fetch, through each cell's
// `subrequest` binding to itself. Every route but /content-type,
// /no-content-type, /status and /redirect answers text/event-stream.

// Captured, as pollution.js patches the prototype while requests arrive.
const headersGet = Headers.prototype.get;
const header = (request, name) => headersGet.call(request.headers, name);

function eventStream(body, init = {}) {
  return new Response(body, {
    ...init,
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
    },
  });
}

// Routes an EventSource reconnects to set the shortest reconnection time,
// one second, to keep the cells fast.

// Per-route request counts, for the routes that answer a reconnection
// differently.
const counts = new Map();
function count(path) {
  const n = counts.get(path) ?? 0;
  counts.set(path, n + 1);
  return n;
}

const routes = {
  // The request headers an EventSource sends.
  '/request-headers': (request) =>
    eventStream(
      `data: ${header(request, 'accept')}|` +
        `${header(request, 'cache-control')}|` +
        `${header(request, 'last-event-id')}\n\n`
    ),

  // Echoes the Last-Event-ID of a reconnection.
  '/last-event-id': (request) => {
    const lastEventId = header(request, 'last-event-id');
    return eventStream(
      lastEventId === null
        ? 'retry: 1000\nid: 1\ndata: first\n\n'
        : `data: ${lastEventId}\n\n`
    );
  },

  // From WPT format-data-before-final-empty-line.any.js.
  '/data-before-final-empty-line': () =>
    eventStream('retry:1000\ndata:test1\n\nid:test\ndata:test2'),

  // Two streams with a body, then responses without one.
  '/reconnect-fail': (request, url) => {
    const n = count(url.pathname);
    if (n === 0) return eventStream('retry: 1000\ndata: opened\n\n');
    if (n === 1) return eventStream('data: reconnected\n\n');
    return eventStream(null, { status: 204 });
  },

  '/retry': () => eventStream('retry: 1000\n\ndata: first\n\n'),

  '/messages': () => eventStream('retry: 1000\ndata: one\n\ndata: two\n\n'),

  '/status': (request, url) =>
    eventStream('data: x\n\n', { status: Number(url.searchParams.get('s')) }),

  '/content-type': (request, url) =>
    new Response('data: x\n\n', {
      headers: { 'Content-Type': url.searchParams.get('ct') },
    }),

  // A byte body gets no default Content-Type.
  '/no-content-type': () =>
    new Response(new TextEncoder().encode('data: x\n\n')),

  '/redirect': () =>
    Response.redirect('http://other.example/request-headers', 302),

  // The stream gc.js's Durable Object reads.
  '/gc': () => eventStream('data: x\n\n'),
};

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const route = routes[url.pathname];
    if (route === undefined) throw new Error(`No route for ${url.pathname}`);
    return route(request, url);
  },
};
