// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import {
  DurableObject,
  WorkerEntrypoint,
  RpcTarget,
  RpcStub,
} from 'cloudflare:workers';

class DisposableTarget extends RpcTarget {
  onDispose;
  disposeCount = 0;

  ping() {
    return 1;
  }

  [Symbol.dispose]() {
    ++this.disposeCount;
    this.onDispose?.();
  }
}

export class TestActor extends DurableObject {
  async hold(stub) {
    assert.strictEqual(await stub.ping(), 1);
    stub[Symbol.dispose]();
  }
}

export class TestService extends WorkerEntrypoint {
  primitive() {
    return 123;
  }
}

export let transferredParamIsDisposedWhileCallIsPending = {
  async test(controller, env, ctx) {
    const target = new DisposableTarget();
    const { promise: disposed, resolve: resolveDisposed } =
      Promise.withResolvers();
    let settled = false;
    let disposedWhilePending = false;
    target.onDispose = () => {
      disposedWhilePending = !settled;
      resolveDisposed();
    };
    const stub = new RpcStub(target);
    const actor = env.TestActor.get(env.TestActor.idFromName('test'));
    const pending = actor.hold(stub);
    Promise.resolve(pending).finally(() => (settled = true));

    await assert.rejects(stub.ping(), /disposed/);
    await disposed;
    assert.strictEqual(disposedWhilePending, true);
    await pending;
    assert.strictEqual(target.disposeCount, 1);
  },
};

export let failedPipelinedCallDoesNotTransferParams = {
  async test(controller, env, ctx) {
    const primitive = ctx.exports.TestService.primitive();
    await primitive;

    const target = new DisposableTarget();
    using stub = new RpcStub(target);
    await assert.rejects(
      primitive.invalid(stub),
      /Can't pipeline on RPC that did not return an object/
    );
    assert.strictEqual(await stub.ping(), 1);
  },
};
