// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import { Buffer } from "node:buffer";

let serializedBody;

export default {
  // Producer receiver (from `env.QUEUE`)
  async fetch(request, env, ctx) {
    assert.strictEqual(request.method, "POST");
    const { pathname } = new URL(request.url);
    if (pathname === "/message") {
      const format = request.headers.get("X-Msg-Fmt") ?? "v8";
      if (format === "text") {
        assert.strictEqual(request.headers.get("X-Msg-Delay-Secs"), "2")
        assert.strictEqual(await request.text(), "abc");
      } else if (format === "bytes") {
        const array = new Uint16Array(await request.arrayBuffer());
        assert.deepStrictEqual(array, new Uint16Array([1, 2, 3]));
      } else if (format === "json") {
        assert.deepStrictEqual(await request.json(), { a: 1 });
      } else if (format === "v8") {
        // workerd doesn't provide V8 deserialization APIs, so just look for expected strings
        const buffer = Buffer.from(await request.arrayBuffer());
        assert(buffer.includes("key"));
        assert(buffer.includes("value"));
        serializedBody = buffer;
      } else {
        assert.fail(`Unexpected format: ${JSON.stringify(format)}`);
      }
    } else if (pathname === "/batch") {
      assert.strictEqual(request.headers.get("X-Msg-Delay-Secs"), "2")

      const body = await request.json();

      assert.strictEqual(typeof body, "object");
      assert(Array.isArray(body?.messages));
      assert.strictEqual(body.messages.length, 4);

      assert.strictEqual(body.messages[0].contentType, "text");
      assert.strictEqual(Buffer.from(body.messages[0].body, "base64").toString(), "def");

      assert.strictEqual(body.messages[1].contentType, "bytes");
      assert.deepStrictEqual(Buffer.from(body.messages[1].body, "base64"), Buffer.from([4, 5, 6]));

      assert.strictEqual(body.messages[2].contentType, "json");
      assert.deepStrictEqual(JSON.parse(Buffer.from(body.messages[2].body, "base64")), [7, 8, { b: 9 }]);

      assert.strictEqual(body.messages[3].contentType, "v8");
      assert(Buffer.from(body.messages[3].body, "base64").includes("value"));
      assert.strictEqual(body.messages[3].delaySecs, 1);
    } else {
      assert.fail(`Unexpected pathname: ${JSON.stringify(pathname)}`);
    }
    return new Response();
  },

  // Consumer receiver (from `env.SERVICE`)
  async queue(batch, env, ctx) {
    assert.strictEqual(batch.queue, "test-queue");
    assert.strictEqual(batch.messages.length, 5);

    assert.strictEqual(batch.messages[0].id, "#0");
    assert.strictEqual(batch.messages[0].body, "ghi");
    assert.strictEqual(batch.messages[0].attempts, 1);

    assert.strictEqual(batch.messages[1].id, "#1");
    assert.deepStrictEqual(batch.messages[1].body, new Uint8Array([7, 8, 9]));
    assert.strictEqual(batch.messages[1].attempts, 2);

    assert.strictEqual(batch.messages[2].id, "#2");
    assert.deepStrictEqual(batch.messages[2].body, { c: { d: 10 } });
    assert.strictEqual(batch.messages[2].attempts, 3);
    batch.messages[2].retry();

    assert.strictEqual(batch.messages[3].id, "#3");
    assert.deepStrictEqual(batch.messages[3].body, batch.messages[3].timestamp);
    assert.strictEqual(batch.messages[3].attempts, 4);
    batch.messages[3].retry({ delaySeconds: 2 });

    assert.strictEqual(batch.messages[4].id, "#4");
    assert.deepStrictEqual(batch.messages[4].body, new Map([["key", "value"]]));
    assert.strictEqual(batch.messages[4].attempts, 5);

    batch.ackAll();
  },

  async test(ctrl, env, ctx) {
    await env.QUEUE.send("abc", { contentType: "text", delaySeconds: 2 });
    await env.QUEUE.send(new Uint16Array([1, 2, 3]), { contentType: "bytes" });
    await env.QUEUE.send({ a: 1 }, { contentType: "json" });
    await env.QUEUE.send(new Map([["key", "value"]]), { contentType: "v8" });

    await env.QUEUE.sendBatch([
      { body: "def", contentType: "text" },
      { body: new Uint8Array([4, 5, 6]), contentType: "bytes" },
      { body: [7, 8, { b: 9 }], contentType: "json" },
      { body: new Set(["value"]), contentType: "v8", delaySeconds: 1 },
    ], { delaySeconds: 2 });

    const timestamp = new Date();
    const response = await env.SERVICE.queue("test-queue", [
      { id: "#0", timestamp, body: "ghi", attempts: 1 },
      { id: "#1", timestamp, body: new Uint8Array([7, 8, 9]), attempts: 2 },
      { id: "#2", timestamp, body: { c: { d: 10 } }, attempts: 3 },
      { id: "#3", timestamp, body: timestamp, attempts: 4 },
      { id: "#4", timestamp, serializedBody, attempts: 5 },
    ]);
    assert.strictEqual(response.outcome, "ok");
    assert(!response.retryBatch.retry);
    assert(response.ackAll);
    assert.deepStrictEqual(response.retryMessages, [{ msgId: '#2' }, { msgId: '#3', delaySeconds: 2 }]);
    assert.deepStrictEqual(response.explicitAcks, []);

    await assert.rejects(env.SERVICE.queue("test-queue", [
      { id: "#0", timestamp, attempts: 1 }
    ]), {
      name: "TypeError",
      message: "Expected one of body or serializedBody for each message"
    });
    await assert.rejects(env.SERVICE.queue("test-queue", [
      { id: "#0", timestamp, body: "", serializedBody, attempts: 1 }
    ]), {
      name: "TypeError",
      message: "Expected one of body or serializedBody for each message"
    });
  },
}
