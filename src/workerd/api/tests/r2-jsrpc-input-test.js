// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

const REMOTE_RELEASED_MESSAGE =
  'ReadableStream sent over RPC was canceled or released by the remote execution context.';
const STREAM_CANCEL_MESSAGE = 'server canceled put body';

function makeObservedStream(chunk) {
  const canceled = Promise.withResolvers();
  let cancelCalls = 0;
  const stream = new ReadableStream({
    start(controller) {
      if (chunk !== undefined) controller.enqueue(chunk);
    },
    cancel(reason) {
      ++cancelCalls;
      canceled.resolve(reason);
    },
  });
  return {
    stream,
    whenCanceled: canceled.promise,
    get cancelCalls() {
      return cancelCalls;
    },
  };
}

async function awaitCancellation(observed) {
  // A timer keeps the caller's execution context alive while the rejected RPC invocation tears
  // down and releases its received stream.
  const pending = Symbol('pending');
  for (let attempt = 0; attempt < 100; attempt++) {
    const outcome = await Promise.race([
      observed.whenCanceled,
      scheduler.wait(50).then(() => pending),
    ]);
    if (outcome !== pending) return outcome;
  }
  assert.fail('the origin never observed the backend releasing the stream');
}

// Verifies that the named-method client does not reject or normalize malformed arguments before
// the custom backend receives them. Each method throws a recognizable server-side error after
// checking its arguments.
export class InputObservingR2Binding extends WorkerEntrypoint {
  get(key, options) {
    assert.strictEqual(key, 'invalid-get');
    assert.strictEqual(options.range.offset, -1);
    throw new Error('server observed get');
  }

  async put(key, value, options) {
    switch (key) {
      case 'invalid-put':
        assert.strictEqual(value, 'body');
        assert.strictEqual(options.md5.byteLength, 1);
        assert(options.httpMetadata instanceof Headers);
        throw new Error('server observed put');
      case 'cancel-stream':
        assert(value instanceof ReadableStream);
        await value.cancel(new Error(STREAM_CANCEL_MESSAGE));
        throw new Error('server canceled stream');
      case 'throw-before-consuming-stream':
        assert(value instanceof ReadableStream);
        throw new Error('server rejected stream before consuming it');
      case 'throw-during-stream': {
        assert(value instanceof ReadableStream);
        const reader = value.getReader();
        const first = await reader.read();
        assert.strictEqual(first.done, false);
        assert.deepStrictEqual(first.value, new Uint8Array([1, 2, 3]));
        reader.releaseLock();
        throw new Error('server rejected stream during consumption');
      }
      default:
        assert.fail(`unexpected put key: ${key}`);
    }
  }

  createMultipartUpload(key, options) {
    assert.strictEqual(key, 'invalid-create');
    assert.strictEqual(options.ssecKey, 'bad');
    throw new Error('server observed createMultipartUpload');
  }

  uploadPart(key, uploadId, partNumber, value, options) {
    assert.strictEqual(key, 'invalid-upload-part');
    assert.strictEqual(uploadId, 'upload-id');
    assert.strictEqual(partNumber, 0);
    assert.strictEqual(value, 'body');
    assert.strictEqual(options.ssecKey, 'bad');
    throw new Error('server observed uploadPart');
  }

  completeMultipartUpload(key, uploadId, uploadedParts) {
    assert.strictEqual(key, 'invalid-complete');
    assert.strictEqual(uploadId, 'upload-id');
    assert.deepStrictEqual(uploadedParts, [{ partNumber: 0, etag: 'invalid' }]);
    throw new Error('server observed completeMultipartUpload');
  }

  list(options) {
    assert.deepStrictEqual(options.include, ['unsupported']);
    throw new Error('server observed list');
  }
}

export const inputForwardingTests = {
  async test(_controller, env) {
    await assert.rejects(
      env.BUCKET.get('invalid-get', { range: { offset: -1 } }),
      { message: 'server observed get' }
    );
    await assert.rejects(
      env.BUCKET.put('invalid-put', 'body', {
        md5: new Uint8Array(1),
        httpMetadata: new Headers({ 'content-type': 'text/plain' }),
      }),
      { message: 'server observed put' }
    );
    await assert.rejects(
      env.BUCKET.createMultipartUpload('invalid-create', { ssecKey: 'bad' }),
      { message: 'server observed createMultipartUpload' }
    );

    const uploadPart = env.BUCKET.resumeMultipartUpload(
      'invalid-upload-part',
      'upload-id'
    );
    await assert.rejects(uploadPart.uploadPart(0, 'body', { ssecKey: 'bad' }), {
      message: 'server observed uploadPart',
    });

    const complete = env.BUCKET.resumeMultipartUpload(
      'invalid-complete',
      'upload-id'
    );
    await assert.rejects(
      complete.complete([{ partNumber: 0, etag: 'invalid' }]),
      { message: 'server observed completeMultipartUpload' }
    );

    // Unsupported includes reach the backend both before and after r2_list_honor_include.
    await assert.rejects(env.BUCKET.list({ include: ['unsupported'] }), {
      message: 'server observed list',
    });

    const explicitlyCanceled = makeObservedStream();
    await assert.rejects(
      env.BUCKET.put('cancel-stream', explicitlyCanceled.stream),
      { message: 'server canceled stream' }
    );
    const explicitReason = await awaitCancellation(explicitlyCanceled);
    assert.strictEqual(explicitlyCanceled.cancelCalls, 1);
    assert(explicitReason instanceof Error);
    assert.strictEqual(explicitReason.message, STREAM_CANCEL_MESSAGE);

    const rejectedBeforeConsumption = makeObservedStream();
    await assert.rejects(
      env.BUCKET.put(
        'throw-before-consuming-stream',
        rejectedBeforeConsumption.stream
      ),
      { message: 'server rejected stream before consuming it' }
    );
    const beforeConsumptionReason = await awaitCancellation(
      rejectedBeforeConsumption
    );
    assert.strictEqual(rejectedBeforeConsumption.cancelCalls, 1);
    assert(beforeConsumptionReason instanceof Error);
    assert.strictEqual(
      beforeConsumptionReason.message,
      REMOTE_RELEASED_MESSAGE
    );

    const rejectedDuringConsumption = makeObservedStream(
      new Uint8Array([1, 2, 3])
    );
    await assert.rejects(
      env.BUCKET.put('throw-during-stream', rejectedDuringConsumption.stream),
      { message: 'server rejected stream during consumption' }
    );
    const duringConsumptionReason = await awaitCancellation(
      rejectedDuringConsumption
    );
    assert.strictEqual(rejectedDuringConsumption.cancelCalls, 1);
    assert(duringConsumptionReason instanceof Error);
    assert.strictEqual(
      duringConsumptionReason.message,
      REMOTE_RELEASED_MESSAGE
    );
  },
};
