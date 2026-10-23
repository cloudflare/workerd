// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// console.log() of a value whose formatting throws, where the thrown value cannot be stringified
// by the formatter's fallback either, must neither throw into the caller nor take the process
// down. Without the C++ exception boundary around the console decorator this aborts workerd.

export default {
  fetch() {
    try {
      console.log({
        get [Symbol.toStringTag]() {
          throw Object.create(null);
        },
      });
    } catch {
      return new Response('caught');
    }
    return new Response('survived');
  },
};

export const test = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://example.com/');
    const body = await response.text();
    if (body !== 'survived') {
      throw new Error(`Expected "survived", got "${body}"`);
    }
  },
};
