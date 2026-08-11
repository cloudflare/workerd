// Tests for the `durable_object_pre_shutdown` compatibility flag's effect on the RPC surface:
// with the flag enabled, `preShutdown` is reserved by the runtime and cannot be called over RPC
// (but user code can still call it directly). The dispatch behavior (the runtime actually
// invoking the hook at shutdown) is tested separately.

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

export class HookActor extends DurableObject {
  async preShutdown(info) {
    return `preShutdown called directly with reason: ${info.reason}`;
  }

  async ping() {
    return 'pong';
  }

  // Per the contract, user code is allowed to invoke the hook method directly; only remote (RPC)
  // invocation is blocked.
  async callPreShutdownDirectly() {
    return this.preShutdown({ reason: 'test' });
  }
}

export class FlagOnEntrypoint extends WorkerEntrypoint {
  async preShutdown() {
    return 'should not be callable over RPC';
  }
}

export default {
  async test(ctrl, env, ctx) {
    let stub = env.HookActor.get(env.HookActor.idFromName('a'));
    assert.strictEqual(await stub.ping(), 'pong');

    // With the flag enabled, `preShutdown` is reserved on Durable Object stubs...
    await assert.rejects(() => stub.preShutdown(), {
      name: 'TypeError',
      message:
        "'preShutdown' is a reserved method and cannot be called over RPC.",
    });

    // ...and on WorkerEntrypoints (the reserved-name list is shared).
    await assert.rejects(() => env.FlagOnEntrypoint.preShutdown(), {
      name: 'TypeError',
      message:
        "'preShutdown' is a reserved method and cannot be called over RPC.",
    });

    // Direct invocation by user code still works.
    assert.strictEqual(
      await stub.callPreShutdownDirectly(),
      'preShutdown called directly with reason: test'
    );

    // The reservation is per-target: a worker without the compat flag exposes `preShutdown` as
    // an ordinary RPC method.
    assert.strictEqual(await env.FlagOff.preShutdown(), 'plain rpc method');
  },
};
