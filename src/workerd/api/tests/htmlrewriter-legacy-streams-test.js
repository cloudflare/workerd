// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, rejects } from 'node:assert';

export const transformWithHandlers = {
  async test() {
    const response = new HTMLRewriter()
      .on('p', {
        async element(element) {
          await scheduler.wait(1);
          element.setAttribute('class', 'x');
          element.append((await fetch('data:,streamed')).body);
        },
      })
      .transform(new Response('<p>hi </p>'));
    strictEqual(await response.text(), '<p class="x">hi streamed</p>');
  },
};

export const handlerError = {
  async test() {
    const response = new HTMLRewriter()
      .on('p', {
        element() {
          throw new TypeError('boom');
        },
      })
      .transform(new Response('<p>hi</p>'));
    await rejects(response.text(), { name: 'TypeError', message: 'boom' });
  },
};

export const cancelBeforeRead = {
  async test() {
    const response = new HTMLRewriter()
      .on('p', { element() {} })
      .transform(new Response('<p>hi</p>'));
    await response.body.cancel();
  },
};
