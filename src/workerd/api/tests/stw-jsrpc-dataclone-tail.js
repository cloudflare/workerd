// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

let jsrpcResult;
let fetchReturnResult;
let queueOnsetResult;

async function captureInfo(info, env) {
  const result = {
    type: info.type,
    protoIsObjectPrototype: Object.getPrototypeOf(info) === Object.prototype,
  };

  if ('statusCode' in info) {
    result.statusCode = info.statusCode;
  }
  if ('batchSize' in info) {
    result.batchSize = info.batchSize;
  }

  try {
    await env.RECEIVER.capture(info);
    result.rpcOk = true;
  } catch (err) {
    result.rpcOk = false;
    result.errorName = err?.name;
    result.errorMessage = err?.message;
  }

  return result;
}

export default {
  async tailStream(onsetEvent, env) {
    const onsetInfo = onsetEvent.event.info;

    if (onsetInfo?.type === 'jsrpc' && jsrpcResult === undefined) {
      jsrpcResult = await captureInfo(onsetInfo, env);
      return;
    }

    if (onsetInfo?.type === 'queue' && queueOnsetResult === undefined) {
      queueOnsetResult = await captureInfo(onsetInfo, env);
      return;
    }

    return async (event) => {
      const info = event.event.info;
      if (
        event.event.type === 'return' &&
        info?.type === 'fetch' &&
        fetchReturnResult === undefined
      ) {
        fetchReturnResult = await captureInfo(info, env);
      }
    };
  },
};

export const test = {
  async test(ctrl, env) {
    await env.RECEIVER.reset();
    jsrpcResult = undefined;
    fetchReturnResult = undefined;
    queueOnsetResult = undefined;

    const response = await env.CALLEE.fetch('http://callee/');
    assert.strictEqual(response.status, 201);
    assert.strictEqual(await response.text(), 'ok');

    const timestamp = new Date();
    const queueResult = await env.CALLEE.queue('stw-jsrpc-dataclone-test', [
      { id: '#0', timestamp, body: 'hello', attempts: 1 },
    ]);
    assert.strictEqual(queueResult.outcome, 'ok');

    assert.strictEqual(await env.CALLEE_RPC.ping(), 'ok');
    await scheduler.wait(100);

    assert.ok(fetchReturnResult, 'missing fetch return info result');
    assert.strictEqual(fetchReturnResult.type, 'fetch');
    assert.strictEqual(fetchReturnResult.statusCode, 201);
    assert.strictEqual(fetchReturnResult.protoIsObjectPrototype, true);
    assert.strictEqual(fetchReturnResult.rpcOk, true);

    assert.ok(queueOnsetResult, 'missing queue onset.info result');
    assert.strictEqual(queueOnsetResult.type, 'queue');
    assert.strictEqual(queueOnsetResult.batchSize, 1);
    assert.strictEqual(queueOnsetResult.protoIsObjectPrototype, true);
    assert.strictEqual(queueOnsetResult.rpcOk, true);

    assert.ok(jsrpcResult, 'missing jsrpc onset.info result');
    assert.strictEqual(jsrpcResult.type, 'jsrpc');
    assert.strictEqual(
      jsrpcResult.protoIsObjectPrototype,
      true,
      'jsrpc onset.info should be a plain object'
    );
    assert.strictEqual(jsrpcResult.rpcOk, true);
  },
};
