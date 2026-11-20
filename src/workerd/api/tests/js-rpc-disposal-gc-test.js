import { strict as assert } from 'node:assert';
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

class Probe extends RpcTarget {
  disposed = false;
  #disposal = Promise.withResolvers();

  poke() {}

  [Symbol.dispose]() {
    this.disposed = true;
    this.#disposal.resolve();
  }

  whenDisposed() {
    return this.#disposal.promise;
  }
}

class Holder extends RpcTarget {
  constructor(ctx) {
    super();
    this.ctx = ctx;
  }

  noop() {}

  abort() {
    this.ctx.abort(new Error('test cleanup'));
  }
}

async function collectGarbage() {
  // Yield before collecting so that the native dispatch's handles no longer root the arguments.
  for (let i = 0; i < 4; ++i) {
    await scheduler.wait(1);
    gc();
  }
}

async function expectDisposed(probe, message) {
  const timeout = scheduler.wait(5000).then(() => {
    throw new Error(message);
  });
  await Promise.race([probe.whenDisposed(), timeout]);
}

export class Service extends WorkerEntrypoint {
  async collectParam(probe, wasDisposed) {
    await probe.poke();
    probe = null;
    await collectGarbage();
    assert.equal(
      await wasDisposed(),
      false,
      'GC must not dispose the parameter'
    );
    // Keep the server context alive after the call, so context teardown cannot mask retention.
    return new Holder(this.ctx);
  }

  forward(probe) {
    return {
      callback: probe.dup(),
      noop() {},
    };
  }

  async collectResultGroup(probe, wasDisposed) {
    let result;
    {
      using forwarded = probe.dup();
      result = await this.env.SERVICE.forward(forwarded);
    }
    // Only the returned callback may retain the probe while its disposal group is collected.
    probe[Symbol.dispose]();
    result.callback = null;
    await collectGarbage();
    result = null;
    await collectGarbage();
    assert.equal(
      await wasDisposed(),
      false,
      'GC of the group must not dispose its stubs'
    );
    return new Holder(this.ctx);
  }
}

export const collectedParam = {
  async test(controller, env) {
    const probe = new Probe();
    using holder = await env.SERVICE.collectParam(probe, () => probe.disposed);
    await expectDisposed(
      probe,
      'collected parameter was not disposed at call completion'
    );
    await holder.noop();
  },
};

export const collectedResultStub = {
  async test(controller, env) {
    const probe = new Probe();
    const result = await env.SERVICE.forward(probe);
    result.callback = null;
    await collectGarbage();
    assert.equal(
      probe.disposed,
      false,
      'GC must not dispose the returned stub'
    );
    await result.noop();
    result[Symbol.dispose]();
    await expectDisposed(
      probe,
      'collected result stub was not disposed with its result'
    );
  },
};

export const collectedResultGroup = {
  async test(controller, env) {
    const probe = new Probe();
    using holder = await env.SERVICE.collectResultGroup(
      probe,
      () => probe.disposed
    );
    assert.equal(probe.disposed, false);
    await assert.rejects(holder.abort(), { message: 'test cleanup' });
    await probe.whenDisposed();
  },
};
