import assert from 'node:assert';
import { WorkerEntrypoint, RpcTarget, RpcStub } from 'cloudflare:workers';

class Counter extends RpcTarget {
  count = { value: 0 };
  dupCounts = { created: 0, disposed: 0 };
  disposeCount = 0;

  increment(amount = 1) {
    this.count.value += amount;
    return this.count.value;
  }

  [Symbol.dispose]() {
    ++this.disposeCount;
  }
}

class DupableCounter extends Counter {
  disposed = false;

  dup() {
    let result = new DupableCounter();
    result.count = this.count;
    result.dupCounts = this.dupCounts;
    ++this.dupCounts.created;
    return result;
  }

  [Symbol.dispose]() {
    if (this.disposed) {
      throw new Error('duplicate disposal');
    }
    this.disposed = true;
    ++this.dupCounts.disposed;
    ++this.disposeCount;
  }
}

export class TestService extends WorkerEntrypoint {
  async increment(stub, i) {
    await stub.increment(i);
  }

  async roundTrip(stub) {
    return { stub: stub.dup() };
  }
}

// Test that (with the rpc_params_dup_stubs compat flag) passing a stub in RPC params doesn't
// transfer ownership of the stub.
export let rpcParamsDontTransferOwnership = {
  async test(controller, env, ctx) {
    let counter = new Counter();

    {
      using stub = new RpcStub(counter);

      // Use the stub in params twice to prove that ownership isn't transferred away.
      await ctx.exports.TestService.increment(stub, 2);
      await ctx.exports.TestService.increment(stub, 3);

      // Make extra-sure we can still call the stub.
      await stub.increment();

      assert.strictEqual(counter.count.value, 6);

      // RpcTarget disposer should not have been called at all.
      await scheduler.wait(0);
      assert.strictEqual(counter.disposeCount, 0);
    }

    // Disposing a stub *asynchrconously* disposes the RpcTarget, so we have to spin the event
    // loop to observe the disposal.
    await scheduler.wait(0);
    assert.strictEqual(counter.disposeCount, 1);
  },
};

// Test that placing a plain RpcTarget in RPC params DOES "take ownership", that is, the disposer
// will be called.
export let rpcParamsPlainTarget = {
  async test(controller, env, ctx) {
    let counter = new Counter();

    await ctx.exports.TestService.increment(counter, 2);
    await ctx.exports.TestService.increment(counter, 3);

    assert.strictEqual(counter.count.value, 5);

    // Each RPC invocation will have called the disposer.
    await scheduler.wait(0);
    assert.strictEqual(counter.disposeCount, 2);
  },
};

// Test that placing an RpcTarget with a dup() method in RPC params causes the dup() method to be
// called, and then the duplicate is later disposed.
export let rpcParamsDupTarget = {
  async test(controller, env, ctx) {
    let counter = new DupableCounter();

    // If we directly pass `counter` to RPC params, it'll be dup()ed.
    await ctx.exports.TestService.increment(counter, 2);
    assert.strictEqual(counter.dupCounts.created, 1);
    await ctx.exports.TestService.increment(counter, 3);
    assert.strictEqual(counter.dupCounts.created, 2);

    assert.strictEqual(counter.count.value, 5);

    // Dups should have been disposed, but not original.
    await scheduler.wait(0);
    assert.strictEqual(counter.dupCounts.disposed, 2);
    assert.strictEqual(counter.disposed, false);
  },
};

// Like rpcParamsDupTarget but the target is wrapped in a Proxy. (This takes a different code
// path.)
export let rpcParamsDupProxyTarget = {
  async test(controller, env, ctx) {
    let counter = new Proxy(new DupableCounter(), {});

    // If we directly pass `counter` to RPC params, it'll be dup()ed.
    await ctx.exports.TestService.increment(counter, 2);
    assert.strictEqual(counter.dupCounts.created, 1);
    await ctx.exports.TestService.increment(counter, 3);
    assert.strictEqual(counter.dupCounts.created, 2);

    assert.strictEqual(counter.count.value, 5);

    // Dups should have been disposed, but not original.
    await scheduler.wait(0);
    assert.strictEqual(counter.dupCounts.disposed, 2);
    assert.strictEqual(counter.disposed, false);
  },
};

// Like rpcParamsDupTarget but the target is a function.
export let rpcParamsDupFunction = {
  async test(controller, env, ctx) {
    let count = 0;
    let dupCount = 0;
    let disposeCount = 0;

    let increment = (i) => {
      return (count += i);
    };
    increment.dup = () => {
      ++dupCount;
      return increment;
    };
    increment[Symbol.dispose] = function () {
      ++disposeCount;
    };

    let counter = { increment };

    // If we directly pass `counter` to RPC params, it'll be dup()ed.
    await ctx.exports.TestService.increment(counter, 2);
    assert.strictEqual(dupCount, 1);
    await ctx.exports.TestService.increment(counter, 3);
    assert.strictEqual(dupCount, 2);

    assert.strictEqual(count, 5);

    await scheduler.wait(0);
    assert.strictEqual(disposeCount, 2);
  },
};

// Test that returning a stub tansfers ownership of the stub, that is, the system later disposes
// it.
export let rpcReturnsTransferOwnership = {
  async test(controller, env, ctx) {
    let counter = new Counter();

    {
      using stub = new RpcStub(counter);
      using stub2 = (await ctx.exports.TestService.roundTrip(stub)).stub;

      await scheduler.wait(0);
      assert.strictEqual(counter.disposeCount, 0);
    }

    await scheduler.wait(0);
    assert.strictEqual(counter.disposeCount, 1);
  },
};
