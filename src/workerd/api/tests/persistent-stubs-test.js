// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  DurableObject,
  WorkerEntrypoint,
  restore,
} from 'cloudflare:workers';

let restoreEvents = [];

function countRestoreEvent(event) {
  return restoreEvents.filter((value) => value === event).length;
}

export class Target extends WorkerEntrypoint {
  ping(value) {
    return `target:${value}`;
  }
}

export class Source extends WorkerEntrypoint {
  async makeService(id) {
    return await this.ctx.restore({ kind: 'service', id });
  }

  async serviceRoundTrip(id, value) {
    let stub = await this.ctx.restore({ kind: 'service', id });
    return await stub.ping(value);
  }

  async serviceStorageRoundTrip(id, value) {
    let store = this.env.Store.get(this.env.Store.idFromName(`service-${id}`));
    await store.put('stub', await this.ctx.restore({ kind: 'service', id }));
    let loaded = await store.get('stub');
    return await loaded.ping(value);
  }

  async makeBad(id) {
    return await this.ctx.restore({ kind: 'bad', id });
  }

  restoreCount(event) {
    return countRestoreEvent(event);
  }

  [restore](params) {
    restoreEvents.push(`${params.kind}:${params.id}`);

    if (params.kind === 'service') {
      return this.env.Target;
    }

    if (params.kind === 'throws') {
      throw new Error('restore failed');
    }

    return 123;
  }
}

export class Consumer extends WorkerEntrypoint {
  async callService(stub, value) {
    return await stub.ping(value);
  }

  async callRpc(stub) {
    return await stub.value();
  }
}

export class Store extends DurableObject {
  async get(name) {
    return await this.ctx.storage.get(name);
  }

  async put(name, value) {
    await this.ctx.storage.put(name, value);
  }
}

export let persistentServiceStubs = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(await env.Source.serviceRoundTrip(1, 'local'), 'target:local');
    assert.strictEqual(await env.Source.restoreCount('service:1'), 2);

    assert.strictEqual(
      await env.Source.serviceStorageRoundTrip(2, 'storage'),
      'target:storage'
    );
    assert.strictEqual(await env.Source.restoreCount('service:2'), 2);
  },
};

export let restoreErrors = {
  async test(ctrl, env, ctx) {
    assert.strictEqual(typeof ctx.restore, 'function');

    await assert.rejects(
      () => Promise.resolve().then(() => ctx.restore([])),
      /restore params must be an object/
    );
    await assert.rejects(
      () => env.Source.makeBad(4),
      /restore method must return a ServiceStub or RpcStub/i
    );
    await assert.rejects(
      () => ctx.restore({ kind: 'throws', id: 4 }),
      /restore failed/
    );
  },

  async [restore](params) {
    throw new Error('restore failed');
  },
};
