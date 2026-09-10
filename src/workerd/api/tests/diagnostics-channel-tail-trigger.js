// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { channel } from 'node:diagnostics_channel';

export default {
  async fetch(request) {
    // Serializable payload: should be forwarded as-is to the tail worker.
    channel('test:serializable').publish({ key: 'value' });

    // Non-serializable payload: a function cannot be structured-cloned.
    // The tail worker should receive a fallback placeholder instead of an
    // exception event.
    channel('test:non-serializable').publish(function notCloneable() {});

    return new Response('ok');
  },
};

export const test = {
  async test(ctrl, env) {
    // Invoke via service binding so the fetch() handler runs in a traced
    // invocation (test() handlers are not traced by the test runner).
    await env.SELF.fetch('http://dummy');
  },
};
