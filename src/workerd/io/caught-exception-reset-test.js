// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// C++ code that catches a JavaScript exception with a v8::TryCatch and then runs more JavaScript
// resets the TryCatch first. Rust JSG treats an exception that a TryCatch has caught and not yet
// reset as pending, and fails while one is. node:url's domainToASCII() is implemented with Rust
// JSG, so it returns undefined, without throwing, if it is called before the reset.
import * as assert from 'node:assert';
import url from 'node:url';

// What each call of the toJSON()s and the getter below returned.
const toJsonResults = [];
const toJsonGetterResults = [];
const retryableResults = [];

export default {
  fetch(request) {
    if (request.url.endsWith('/log')) {
      const cyclic = {};
      cyclic.self = cyclic;
      // Stringifying `cyclic` for the trace throws, and is caught. The next argument is then
      // stringified, which calls its toJSON().
      console.log(cyclic, {
        toJSON() {
          toJsonResults.push(url.domainToASCII('example.com'));
          return 'x';
        },
      });
      return new Response('ok');
    }

    if (request.url.endsWith('/log-getter')) {
      let calls = 0;
      // Checking whether the argument has a toJSON() function calls this getter, which throws
      // the first time, and is caught. Stringifying the argument then calls it again.
      console.log({
        get toJSON() {
          if (calls++ === 0) throw new Error('first toJSON lookup');
          return () => {
            toJsonGetterResults.push(url.domainToASCII('example.com'));
            return 'x';
          };
        },
      });
      return new Response('ok');
    }

    // Thrown synchronously, so that IoContext catches it. Converting it to a KJ exception reads
    // its `retryable` property, and so does serializing it afterwards.
    throw {
      get retryable() {
        retryableResults.push(url.domainToASCII('example.com'));
        return false;
      },
    };
  },

  tail() {},
};

export const consoleLogAfterStringifyThrows = {
  async test(ctrl, env) {
    const response = await env.SERVICE.fetch('http://example.com/log');
    assert.strictEqual(await response.text(), 'ok');
    assert.ok(toJsonResults.length > 0);
    for (const result of toJsonResults) {
      assert.strictEqual(result, 'example.com');
    }
  },
};

export const consoleLogAfterToJsonGetterThrows = {
  async test(ctrl, env) {
    const response = await env.SERVICE.fetch('http://example.com/log-getter');
    assert.strictEqual(await response.text(), 'ok');
    assert.ok(toJsonGetterResults.length > 0);
    for (const result of toJsonGetterResults) {
      assert.strictEqual(result, 'example.com');
    }
  },
};

export const uncaughtExceptionGetter = {
  async test(ctrl, env) {
    await assert.rejects(env.SERVICE.fetch('http://example.com/throw'));
    assert.ok(retryableResults.length > 0);
    for (const result of retryableResults) {
      assert.strictEqual(result, 'example.com');
    }
  },
};
