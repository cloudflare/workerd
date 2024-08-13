// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import util from "node:util";

let scheduledLastCtrl;

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === "/body-length") {
      return Response.json(Object.fromEntries(request.headers));
    }
    if (pathname === "/web-socket") {
      const pair = new WebSocketPair();
      pair[0].addEventListener("message", (event) => {
        pair[0].send(util.inspect(event));
      });
      pair[0].accept();
      return new Response(null, {
        status: 101,
        webSocket: pair[1],
      });
    }
    return new Response(null, { status: 404 });
  },

  async scheduled(ctrl, env, ctx) {
    scheduledLastCtrl = ctrl;
    if (ctrl.cron === "* * * * 30") ctrl.noRetry();
  },

  async test(ctrl, env, ctx) {
    // Call `fetch()` with known body length
    {
      const body = new FixedLengthStream(3);
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch("http://placeholder/body-length", {
        method: "POST",
        body: body.readable,
      });
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get("Content-Length"), "3");
      assert.strictEqual(headers.get("Transfer-Encoding"), null);
    }

    // Check `fetch()` with unknown body length
    {
      const body = new IdentityTransformStream();
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch("http://placeholder/body-length", {
        method: "POST",
        body: body.readable,
      });
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get("Content-Length"), null);
      assert.strictEqual(headers.get("Transfer-Encoding"), "chunked");
    }

    // Call `scheduled()` with no options
    {
      const result = await env.SERVICE.scheduled();
      assert.strictEqual(result.outcome, "ok");
      assert(!result.noRetry);
      assert(Math.abs(Date.now() - scheduledLastCtrl.scheduledTime) < 3_000);
      assert.strictEqual(scheduledLastCtrl.cron, "");
    }

    // Call `scheduled()` with options, and noRetry()
    {
      const result = await env.SERVICE.scheduled({ scheduledTime: 1000, cron: "* * * * 30" });
      assert.strictEqual(result.outcome, "ok");
      assert(result.noRetry);
      assert.strictEqual(scheduledLastCtrl.scheduledTime, 1000);
      assert.strictEqual(scheduledLastCtrl.cron, "* * * * 30");
    }
  }
}

export const inspect = {
  async test(ctrl, env, ctx) {
    // Check URL with duplicate search param keys
    const url = new URL("http://user:pass@placeholder:8787/path?a=1&a=2&b=3");
    assert.strictEqual(util.inspect(url),
      `URL {
  origin: 'http://placeholder:8787',
  href: 'http://user:pass@placeholder:8787/path?a=1&a=2&b=3',
  protocol: 'http:',
  username: 'user',
  password: 'pass',
  host: 'placeholder:8787',
  hostname: 'placeholder',
  port: '8787',
  pathname: '/path',
  search: '?a=1&a=2&b=3',
  hash: '',
  searchParams: URLSearchParams(3) { 'a' => '1', 'a' => '2', 'b' => '3' }
}`
    );

    // Check FormData with lower depth
    const formData = new FormData();
    formData.set("string", "hello");
    formData.set("blob", new Blob(["<h1>BLOB</h1>"], {
      type: "text/html"
    }));
    formData.set("file", new File(["password123"], "passwords.txt", {
      type: "text/plain",
      lastModified: 1000
    }));
    assert.strictEqual(util.inspect(formData, { depth: 0 }),
      `FormData(3) { 'string' => 'hello', 'blob' => [File], 'file' => [File] }`
    );

    // Check request with mutable headers
    const request = new Request("http://placeholder", {
      method: "POST",
      body: "message",
      headers: { "Content-Type": "text/plain" }
    });
    assert.strictEqual(util.inspect(request),
      `Request {
  method: 'POST',
  url: 'http://placeholder',
  headers: Headers(1) { 'content-type' => 'text/plain', [immutable]: false },
  redirect: 'follow',
  fetcher: null,
  signal: AbortSignal { aborted: false, reason: undefined, onabort: null },
  cf: undefined,
  integrity: '',
  keepalive: false,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 7n
  },
  bodyUsed: false
}`
    );

    // Check response with immutable headers
    const response = await env.SERVICE.fetch("http://placeholder/not-found");
    assert.strictEqual(util.inspect(response),
      `Response {
  status: 404,
  statusText: 'Not Found',
  headers: Headers(0) { [immutable]: true },
  ok: false,
  redirected: false,
  url: 'http://placeholder/not-found',
  webSocket: null,
  cf: undefined,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 0n
  },
  bodyUsed: false
}`
    );

    // Check `MessageEvent` with unimplemented properties
    const webSocketResponse = await env.SERVICE.fetch("http://placeholder/web-socket", {
      headers: { "Upgrade": "websocket" },
    });
    const webSocket = webSocketResponse.webSocket;
    assert.notStrictEqual(webSocket, null);
    const messagePromise = new Promise((resolve) => {
      webSocket.addEventListener("message", (event) => {
        assert.strictEqual(event.data,
          `MessageEvent {
  data: 'data',
  type: 'message',
  eventPhase: 2,
  composed: false,
  bubbles: false,
  cancelable: false,
  defaultPrevented: false,
  returnValue: true,
  currentTarget: WebSocket { readyState: 1, url: null, protocol: '', extensions: '' },
  srcElement: WebSocket { readyState: 1, url: null, protocol: '', extensions: '' },
  timeStamp: 0,
  isTrusted: true,
  cancelBubble: false,
  NONE: 0,
  CAPTURING_PHASE: 1,
  AT_TARGET: 2,
  BUBBLING_PHASE: 3
}`
        );
        resolve();
      });
    });
    webSocket.accept();
    webSocket.send("data");
    webSocket.close();
    await messagePromise;
  }
};

async function assertRequestCacheThrowsError(cacheHeader,
  errorName = 'Error',
  errorMessage = "The 'cache' field on 'RequestInitializerDict' is not implemented.") {
  assert.throws(() => {
    new Request('https://example.org', { cache: cacheHeader });
  }, {
    name: errorName,
    message: errorMessage,
  });
}

async function assertFetchCacheRejectsError(cacheHeader,
  errorName = 'Error',
  errorMessage = "The 'cache' field on 'RequestInitializerDict' is not implemented.") {
  await assert.rejects((async () => {
    await fetch('http://example.org', { cache: cacheHeader });
  })(), {
    name: errorName,
    message: errorMessage,
  });
}

export const cacheMode = {

  async test() {
    assert.strictEqual("cache" in Request.prototype, false);
    {
      const req = new Request('https://example.org', {});
      assert.strictEqual(req.cache, undefined);
    }
    await assertRequestCacheThrowsError('no-store');
    await assertRequestCacheThrowsError('no-cache');
    await assertRequestCacheThrowsError('no-transform');
    await assertRequestCacheThrowsError('unsupported');
    await assertFetchCacheRejectsError('no-store');
    await assertFetchCacheRejectsError('no-cache');
    await assertFetchCacheRejectsError('no-transform');
    await assertFetchCacheRejectsError('unsupported');
  }
}
