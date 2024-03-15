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
  searchParams: URLSearchParams(3) { 'a' => '1', 'a' => '2', 'b' => '3' },
  hash: '',
  search: '?a=1&a=2&b=3',
  pathname: '/path',
  port: '8787',
  hostname: 'placeholder',
  host: 'placeholder:8787',
  password: 'pass',
  username: 'user',
  protocol: 'http:',
  href: 'http://user:pass@placeholder:8787/path?a=1&a=2&b=3',
  origin: 'http://placeholder:8787'
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
  keepalive: false,
  integrity: '',
  cf: undefined,
  signal: AbortSignal { onabort: null, reason: undefined, aborted: false },
  fetcher: null,
  redirect: 'follow',
  headers: Headers(1) { 'content-type' => 'text/plain', [immutable]: false },
  url: 'http://placeholder',
  method: 'POST',
  bodyUsed: false,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 7n
  }
}`
    );

    // Check response with immutable headers
    const response = await env.SERVICE.fetch("http://placeholder/not-found");
    assert.strictEqual(util.inspect(response),
`Response {
  cf: undefined,
  webSocket: null,
  url: 'http://placeholder/not-found',
  redirected: false,
  ok: false,
  headers: Headers(0) { [immutable]: true },
  statusText: 'Not Found',
  status: 404,
  bodyUsed: false,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 0n
  }
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
  cancelBubble: false,
  isTrusted: true,
  timeStamp: 0,
  srcElement: WebSocket { extensions: '', protocol: '', url: null, readyState: 1 },
  currentTarget: WebSocket { extensions: '', protocol: '', url: null, readyState: 1 },
  returnValue: true,
  defaultPrevented: false,
  cancelable: false,
  bubbles: false,
  composed: false,
  eventPhase: 2,
  type: 'message',
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
