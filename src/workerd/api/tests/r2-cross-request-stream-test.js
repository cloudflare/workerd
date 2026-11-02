// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// R2Bucket.put() cancels its stream body when the put fails before the body is sent. A stream
// owned by another request can't be canceled from this one, and that failure must not escape
// the cleanup, which runs while the put's own error is propagating.

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

let foreignStream;

export class StreamOwner extends WorkerEntrypoint {
  // Each RPC call runs in its own request, so the stream is owned by that request.
  createStream() {
    foreignStream = new Response('content').body;
  }
}

const crossRequestError = {
  name: 'Error',
  message: /^Cannot perform I\/O on behalf of a different request\./,
};

export const putFailsOnForeignStream = {
  async test(ctrl, env) {
    await env.STREAM_OWNER.createStream();
    // Reading the stream's length fails, and so does the cleanup's attempt to cancel it.
    await assert.rejects(
      env.BUCKET.put('key', foreignStream),
      crossRequestError
    );
  },
};

export const putReportsOriginalErrorOnForeignStream = {
  async test(ctrl, env) {
    await env.STREAM_OWNER.createStream();
    // The put fails validating the checksum. Canceling the stream fails too, but the put
    // reports the original error.
    await assert.rejects(env.BUCKET.put('key', foreignStream, { md5: 'bad' }), {
      name: 'TypeError',
      message: 'MD5 is 32 hex characters, not 3',
    });
  },
};
