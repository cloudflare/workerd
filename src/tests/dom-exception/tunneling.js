// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DOMException crossing to another worker comes back as a DOMException
// with its name and message: through the kj::Exception it tunnels as
// (`jsg.DOMException(Name): message`), or serialized over RPC.

import { rejects, strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';
import { assertDomException } from 'helpers';

export const thrownFromAFetchHandler = {
  async test() {
    await rejects(env.SELF.fetch('http://dom-exception/throw'), (error) => {
      assertDomException(
        error,
        { name: 'InvalidStateError', message: 'thrown', code: 11 },
        'remote'
      );
      strictEqual(error.remote, true);
      return true;
    });
  },
};

export const thrownOverRpc = {
  async test() {
    await rejects(env.RPC.throwDomException(), (error) => {
      assertDomException(
        error,
        { name: 'NotFoundError', message: 'thrown', code: 8 },
        'remote'
      );
      return true;
    });
  },
};

export const returnedOverRpc = {
  async test() {
    const error = await env.RPC.returnDomException();
    // RPC gives the objects it returns a disposer.
    assertDomException(
      error,
      { name: 'AbortError', message: 'returned', code: 20 },
      'Symbol(Symbol.dispose)'
    );
    strictEqual(error.extra, undefined);
  },
};

export const erroredStreams = {
  async test() {
    const stream = new ReadableStream({
      pull(controller) {
        controller.error(new DOMException('errored', 'DataError'));
      },
    });
    await rejects(new Response(stream).text(), (error) => {
      assertDomException(error, {
        name: 'DataError',
        message: 'errored',
        code: 0,
      });
      return true;
    });
  },
};
