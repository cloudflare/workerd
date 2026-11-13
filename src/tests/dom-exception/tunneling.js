// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DOMException crossing to another worker comes back as a DOMException
// with its name and message: through the kj::Exception it tunnels as
// (`jsg.DOMException(Name): message`), or serialized over RPC.

import { deepStrictEqual, rejects, strictEqual } from 'node:assert';
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

// An aborted AbortSignal's reason is deserialized with it, and the signal
// aborted with it, while V8 forbids JavaScript: reading the reason's name
// must not run the attribute's getter.
export const abortReasonsOverRpc = {
  async test() {
    const reason = new DOMException('m', 'NotFoundError');
    deepStrictEqual(
      await env.RPC.describeAbortReason(AbortSignal.abort(reason)),
      [true, 'NotFoundError', 'm', 8, 'string']
    );
    deepStrictEqual(await env.RPC.describeAbortReason(AbortSignal.abort()), [
      true,
      'AbortError',
      'The operation was aborted',
      20,
      'string',
    ]);
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
