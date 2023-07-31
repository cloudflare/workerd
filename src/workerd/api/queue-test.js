// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import { Buffer } from "node:buffer";

export default {
  // Producer receiver (from `env.QUEUE`)
  async fetch(request, env, ctx) {
    assert.strictEqual(request.method, "POST");
    const { pathname } = new URL(request.url);
    if (pathname === "/message") {
      const format = request.headers.get("X-Msg-Fmt") ?? "v8";
      if (format === "text") {
        assert.strictEqual(await request.text(), "abc");
      } else if (format === "bytes") {
        const array = new Uint16Array(await request.arrayBuffer());
        assert.deepStrictEqual(array, new Uint16Array([1, 2, 3]));
      } else if (format === "json") {
        assert.deepStrictEqual(await request.json(), {a: 1});
      } else if (format === "v8") {
        // workerd doesn't provide V8 deserialization APIs, so just look for expected strings
        const buffer = Buffer.from(await request.arrayBuffer());
        assert(buffer.includes("key"));
        assert(buffer.includes("value"));
      } else {
        assert.fail(`Unexpected format: ${JSON.stringify(format)}`);
      }
    } else if (pathname === "/batch") {
      const body = await request.json();

      assert.strictEqual(typeof body, "object");
      assert(Array.isArray(body?.messages));
      assert.strictEqual(body.messages.length, 4);

      assert.strictEqual(body.messages[0].contentType, "text");
      assert.strictEqual(Buffer.from(body.messages[0].body, "base64").toString(), "def");

      assert.strictEqual(body.messages[1].contentType, "bytes");
      assert.deepStrictEqual(Buffer.from(body.messages[1].body, "base64"), Buffer.from([4, 5, 6]));

      assert.strictEqual(body.messages[2].contentType, "json");
      assert.deepStrictEqual(JSON.parse(Buffer.from(body.messages[2].body, "base64")), [7, 8, {b: 9}]);

      assert.strictEqual(body.messages[3].contentType, "v8");
      assert(Buffer.from(body.messages[3].body, "base64").includes("value"));
    } else {
      assert.fail(`Unexpected pathname: ${JSON.stringify(pathname)}`);
    }
    return new Response();
  },

  // Consumer receiver (from `env.SERVICE`)
  async queue(batch, env, ctx) {
    assert.strictEqual(batch.queue, "test-queue");
    assert.strictEqual(batch.messages.length, 4);

    assert.strictEqual(batch.messages[0].id, "#0");
    assert.strictEqual(batch.messages[0].body, "ghi");

    assert.strictEqual(batch.messages[1].id, "#1");
    assert.deepStrictEqual(batch.messages[1].body, new Uint8Array([7, 8, 9]));

    assert.strictEqual(batch.messages[2].id, "#2");
    assert.deepStrictEqual(batch.messages[2].body, { c: {d: 10 } });
    batch.messages[2].retry();

    assert.strictEqual(batch.messages[3].id, "#3");
    assert.deepStrictEqual(batch.messages[3].body, batch.messages[3].timestamp);

    batch.ackAll();
  },

  async test(ctrl, env, ctx) {
    await env.QUEUE.send("abc", { contentType: "text" });
    await env.QUEUE.send(new Uint16Array([1, 2, 3]), { contentType: "bytes" });
    await env.QUEUE.send({a: 1}, { contentType: "json" });
    await env.QUEUE.send(new Map([["key", "value"]]), { contentType: "v8" });

    await env.QUEUE.sendBatch([
      { body: "def", contentType: "text" },
      { body: new Uint8Array([4,5,6]), contentType: "bytes" },
      { body: [7, 8, {b: 9}], contentType: "json" },
      { body: new Set(["value"]), contentType: "v8" },
    ]);

    const timestamp = new Date();
    const response = await env.SERVICE.queue("test-queue", [
      { id: "#0", timestamp, body: "ghi" },
      { id: "#1", timestamp, body: new Uint8Array([7, 8, 9]) },
      { id: "#2", timestamp, body: { c: { d: 10 } } },
      { id: "#3", timestamp, body: timestamp },
    ]);
    assert.strictEqual(response.outcome, /* OK */ 1);
    assert(!response.retryAll);
    assert(response.ackAll);
    assert.deepStrictEqual(response.explicitRetries, ["#2"]);
    assert.deepStrictEqual(response.explicitAcks, []);
  },
}
