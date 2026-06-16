// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  DurableObject,
  RpcStub,
  RpcTarget,
  WorkerEntrypoint,
  restore,
} from 'cloudflare:workers';

let restoreEvents = [];

function countRestoreEvent(event) {
  return restoreEvents.filter((value) => value === event).length;
}

let DYNAMIC_WORKER_CODE = `
import { WorkerEntrypoint } from 'cloudflare:workers';
export class Target extends WorkerEntrypoint {
  ping(value) {
    return \`target:\${value}\`;
  }
}
`;

let NESTED_DYNAMIC_WORKER_CODE = `
import { WorkerEntrypoint, restore } from 'cloudflare:workers';

export class Outer extends WorkerEntrypoint {
  async makeInner() {
    return await this.ctx.restore({ kind: 'inner', id: this.ctx.props.id });
  }

  async callInner(value) {
    let inner = await this.makeInner();
    return await inner.ping(value);
  }

  async [restore](params) {
    return this.ctx.exports.Inner({ props: { id: params.id } });
  }
}

export class Inner extends WorkerEntrypoint {
  ping(value) {
    return \`nested-dynamic:\${this.ctx.props.id}:\${value}\`;
  }
}
`;

class PersistentCounter extends RpcTarget {
  constructor(id) {
    super();
    this.id = id;
    this.value = 0;
  }

  add(amount) {
    this.value += amount;
    return `counter:${this.id}:${this.value}`;
  }
}

class CapabilityUsingTarget extends RpcTarget {
  constructor(service, counter) {
    super();
    this.service = service;
    this.counter = counter;
  }

  async combine(value) {
    let serviceResult = await this.service.ping(value);
    let counterResult = await this.counter.add(3);
    return `${serviceResult}|${counterResult}`;
  }
}

export class StaticTarget extends WorkerEntrypoint {
  ping(value) {
    return `${this.ctx.props.prefix}:${value}`;
  }
}

export class StaticRedirect extends WorkerEntrypoint {
  async makeInner(id) {
    return await this.ctx.restore({ kind: 'static-inner', id });
  }

  async [restore](params) {
    restoreEvents.push(`static:${params.kind}:${params.id}`);
    return this.ctx.exports.StaticTarget({
      props: { prefix: `static:${params.id}` },
    });
  }
}

export class Receiver extends WorkerEntrypoint {
  async storeAndCall(id, stub, amount) {
    let store = this.ctx.exports.Store.getByName(`receiver-${id}`);
    await store.put('stub', stub);
    let loaded = await store.get('stub');
    return await loaded.add(amount);
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
    let store = this.ctx.exports.Store.getByName(`service-${id}`);
    let orig = await this.ctx.restore({ kind: 'service', id });
    await store.put('stub', orig);
    let loaded = await store.get('stub');
    return await loaded.ping(value);
  }

  async makeBad(id) {
    return await this.ctx.restore({ kind: 'bad', id });
  }

  async rpcRoundTrip(id, amount) {
    let stub = await this.ctx.restore({ kind: 'rpc', id });
    return await stub.add(amount);
  }

  async rpcStorageRoundTrip(id, amount) {
    let store = this.ctx.exports.Store.getByName(`rpc-${id}`);
    return await store.rpcStorageRoundTrip('rpc', id, amount);
  }

  async rpcDupStorageRoundTrip(id, amount) {
    let store = this.ctx.exports.Store.getByName(`rpc-dup-${id}`);
    return await store.rpcDupStorageRoundTrip(id, amount);
  }

  async implicitRpcStorageRoundTrip(kind, id, amount) {
    let store = this.ctx.exports.Store.getByName(`${kind}-${id}`);
    return await store.rpcStorageRoundTrip(kind, id, amount);
  }

  async nestedDynamicStorageRoundTrip(id, value) {
    let store = this.ctx.exports.Store.getByName(`nested-dynamic-${id}`);
    let outer = await this.ctx.restore({ kind: 'nested-dynamic', id });
    let inner = await outer.makeInner();
    await store.put('stub', inner);
    let loaded = await store.get('stub');
    return await loaded.ping(value);
  }

  async nestedDynamicImmediate(id, value) {
    let outer = await this.ctx.restore({ kind: 'nested-dynamic', id });
    return await outer.callInner(value);
  }

  async directDynamicRestoreError(id) {
    let worker = this.env.LOADER.get(`direct-dynamic-${id}`, () => {
      return {
        compatibilityDate: '2025-01-01',
        compatibilityFlags: [
          'allow_irrevocable_stub_storage',
          'experimental',
          'enable_ctx_exports',
        ],
        allowExperimental: true,
        mainModule: 'foo.js',
        modules: { 'foo.js': NESTED_DYNAMIC_WORKER_CODE },
      };
    });

    return await worker
      .getEntrypoint('Outer', { props: { id } })
      .callInner('direct');
  }

  async rpcTransferStorageRoundTrip(id, amount) {
    let stub = await this.ctx.restore({ kind: 'rpc', id });
    return await this.ctx.exports.Receiver.storeAndCall(id, stub, amount);
  }

  async dynamicEnvRpcRoundTrip(id, amount) {
    let counter = await this.ctx.restore({ kind: 'rpc', id });
    let worker = this.env.LOADER.get(`dynamic-env-rpc-${id}`, () => {
      return {
        compatibilityDate: '2025-01-01',
        compatibilityFlags: ['allow_irrevocable_stub_storage', 'experimental'],
        allowExperimental: true,
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            import { WorkerEntrypoint } from 'cloudflare:workers';
            export class Runner extends WorkerEntrypoint {
              run(amount) {
                return this.env.counter.add(amount);
              }
            }
          `,
        },
        env: { counter },
      };
    });

    return await worker.getEntrypoint('Runner').run(amount);
  }

  async dynamicPropsRpcRoundTrip(id, amount) {
    let counter = await this.ctx.restore({ kind: 'rpc', id });
    let worker = this.env.LOADER.get(`dynamic-props-rpc-${id}`, () => {
      return {
        compatibilityDate: '2025-01-01',
        compatibilityFlags: ['allow_irrevocable_stub_storage', 'experimental'],
        allowExperimental: true,
        mainModule: 'foo.js',
        modules: {
          'foo.js': `
            import { WorkerEntrypoint } from 'cloudflare:workers';
            export class Runner extends WorkerEntrypoint {
              run(amount) {
                return this.ctx.props.counter.add(amount);
              }
            }
          `,
        },
      };
    });

    return await worker
      .getEntrypoint('Runner', { props: { counter } })
      .run(amount);
  }

  async restoreArgsWithCapabilities(id, value) {
    let store = this.ctx.exports.Store.getByName(`args-caps-${id}`);
    return await store.restoreArgsWithCapabilities(id, value);
  }

  async serviceThenRpcMismatch(id) {
    let store = this.ctx.exports.Store.getByName(`service-rpc-mismatch-${id}`);
    let stub = await this.ctx.restore({ kind: 'service-then-rpc', id });
    await store.put('stub', stub);
    restoreEvents.push(`flip:${id}`);
    let loaded = await store.get('stub');
    return await loaded.ping('mismatch');
  }

  async rpcThenServiceMismatch(id) {
    let store = this.ctx.exports.Store.getByName(`rpc-service-mismatch-${id}`);
    let stub = await this.ctx.restore({ kind: 'rpc-then-service', id });
    await store.put('stub', stub);
    restoreEvents.push(`flip:${id}`);
    let loaded = await store.get('stub');
    return await loaded.add(1);
  }

  async plainRpcStorageError(id) {
    let store = this.ctx.exports.Store.getByName(`plain-rpc-${id}`);
    await store.put('stub', new RpcStub(new PersistentCounter(id)));
  }

  async staticRedirectSecurityRoundTrip(id, value) {
    let store = this.ctx.exports.Store.getByName(`static-redirect-${id}`);
    let redirected = await this.ctx.restore({
      kind: 'static-redirect',
      id,
      secret: 'source-only',
    });
    let inner = await redirected.makeInner(id);
    await store.put('stub', inner);
    let loaded = await store.get('stub');
    return await loaded.ping(value);
  }

  async topLevelDoRedirectSecurityRoundTrip(id, value) {
    let store = this.ctx.exports.Store.getByName(`top-do-redirect-${id}`);
    let redirected = await this.ctx.restore({
      kind: 'top-do-redirect',
      id,
      secret: 'source-only',
    });
    let inner = await redirected.makeInner(id);
    await store.put('stub', inner);
    let loaded = await store.get('stub');
    return await loaded.ping(value);
  }

  restoreCount(event) {
    return countRestoreEvent(event);
  }

  async [restore](params) {
    restoreEvents.push(`${params.kind}:${params.id}`);

    if (params.kind === 'service') {
      let worker = this.env.LOADER.get('service', () => {
        return {
          compatibilityDate: '2025-01-01',
          mainModule: 'foo.js',
          modules: {
            'foo.js': DYNAMIC_WORKER_CODE,
          },
        };
      });

      return worker.getEntrypoint('Target');
    }

    if (params.kind === 'rpc') {
      return new RpcStub(new PersistentCounter(params.id));
    }

    if (params.kind === 'implicit-rpc-target') {
      return new PersistentCounter(params.id);
    }

    if (params.kind === 'implicit-rpc-function') {
      let counter = new PersistentCounter(params.id);
      let fn = (amount) => counter.add(amount);
      fn.add = (amount) => counter.add(amount);
      return fn;
    }

    if (params.kind === 'nested-dynamic') {
      let worker = this.env.LOADER.get(`nested-dynamic-${params.id}`, () => {
        return {
          compatibilityDate: '2025-01-01',
          compatibilityFlags: [
            'allow_irrevocable_stub_storage',
            'experimental',
            'enable_ctx_exports',
          ],
          allowExperimental: true,
          mainModule: 'foo.js',
          modules: { 'foo.js': NESTED_DYNAMIC_WORKER_CODE },
        };
      });

      return worker.getEntrypoint('Outer', { props: { id: params.id } });
    }

    if (params.kind === 'args-caps') {
      return new RpcStub(
        new CapabilityUsingTarget(params.service, params.counter)
      );
    }

    if (params.kind === 'service-then-rpc') {
      if (countRestoreEvent(`flip:${params.id}`) > 0) {
        return new RpcStub(new PersistentCounter(params.id));
      }
      return this.ctx.exports.StaticTarget({
        props: { prefix: `mismatch-service:${params.id}` },
      });
    }

    if (params.kind === 'rpc-then-service') {
      if (countRestoreEvent(`flip:${params.id}`) > 0) {
        return this.ctx.exports.StaticTarget({
          props: { prefix: `mismatch-service:${params.id}` },
        });
      }
      return new RpcStub(new PersistentCounter(params.id));
    }

    if (params.kind === 'static-redirect') {
      return this.ctx.exports.StaticRedirect({
        props: { sourceId: params.id },
      });
    }

    if (params.kind === 'top-do-redirect') {
      let id = this.ctx.exports.TopRedirectDo.idFromName(
        `redirect-${params.id}`
      );
      return this.ctx.exports.TopRedirectDo.get(id);
    }

    if (params.kind === 'throws') {
      throw new Error('restore failed');
    }

    return 123;
  }
}

export class Store extends DurableObject {
  async get(name) {
    return await this.ctx.storage.get(name);
  }

  async put(name, value) {
    await this.ctx.storage.put(name, value);
  }

  async rpcStorageRoundTrip(kind, id, amount) {
    let orig = await this.ctx.restore({ kind, id });
    assert.strictEqual(orig instanceof RpcStub, true);
    await this.put('stub', orig);
    let loaded = await this.ctx.storage.get('stub');
    return await loaded.add(amount);
  }

  async rpcDupStorageRoundTrip(id, amount) {
    let orig = await this.ctx.restore({ kind: 'rpc', id });
    await this.put('stub', orig.dup());
    let loaded = await this.ctx.storage.get('stub');
    return await loaded.add(amount);
  }

  async restoreArgsWithCapabilities(id, value) {
    let service = this.ctx.exports.StaticTarget({
      props: { prefix: `arg-service:${id}` },
    });
    let counter = await this.ctx.restore({
      kind: 'rpc',
      id: `arg-counter-${id}`,
    });
    let orig = await this.ctx.restore({
      kind: 'args-caps',
      id,
      service,
      counter,
    });
    await this.put('stub', orig);
    let loaded = await this.ctx.storage.get('stub');
    return await loaded.combine(value);
  }

  // Kick off a ctx.restore() from a waitUntil() task that resumes *after* this request has
  // returned, i.e. while the actor is draining and no client is waiting for a response. This is a
  // regression test: previously the actor's self-channel was dropped during drain() (to avoid a
  // reference cycle that blocked eviction), which made ctx.restore() fail in exactly this
  // situation. The result is stashed in storage so a later call can observe it.
  async startRestoreInWaitUntil(id, amount) {
    await this.ctx.storage.delete('waitUntilResult');
    this.ctx.waitUntil(
      (async () => {
        // Yield to the event loop so this resumes after the response is delivered and the actor
        // has started draining (with no incoming request -- hence no self-channel -- otherwise).
        await new Promise((resolve) => setTimeout(resolve, 0));
        let stub = await this.ctx.restore({ kind: 'rpc', id });
        await this.ctx.storage.put('waitUntilResult', await stub.add(amount));
      })()
    );
    return 'started';
  }

  async getWaitUntilResult() {
    return await this.ctx.storage.get('waitUntilResult');
  }

  async [restore](params) {
    restoreEvents.push(`${params.kind}:${params.id}`);

    if (params.kind === 'rpc') {
      return new RpcStub(new PersistentCounter(params.id));
    }

    if (params.kind === 'implicit-rpc-target') {
      return new PersistentCounter(params.id);
    }

    if (params.kind === 'implicit-rpc-function') {
      let counter = new PersistentCounter(params.id);
      let fn = (amount) => counter.add(amount);
      fn.add = (amount) => counter.add(amount);
      return fn;
    }

    if (params.kind === 'args-caps') {
      return new RpcStub(
        new CapabilityUsingTarget(params.service, params.counter)
      );
    }

    return 123;
  }
}

export class RestorableFacet extends DurableObject {
  ping(value) {
    return `${this.ctx.props.prefix}:${value}`;
  }

  async makeInner(id) {
    return await this.ctx.restore({ kind: 'facet-inner', id });
  }

  async [restore](params) {
    restoreEvents.push(`facet:${params.kind}:${params.id}`);
    return this.ctx.exports.StaticTarget({
      props: { prefix: `facet-inner:${params.id}` },
    });
  }
}

export class FacetRoot extends DurableObject {
  async facetStorageRoundTrip(id, value) {
    let stub = await this.ctx.restore({ kind: 'facet', id });
    await this.ctx.storage.put('stub', stub);
    let loaded = await this.ctx.storage.get('stub');
    return await loaded.ping(value);
  }

  async nestedFacetStorageRoundTrip(id, value) {
    let facet = await this.ctx.restore({ kind: 'facet', id });
    let inner = await facet.makeInner(id);
    await this.ctx.storage.put('inner', inner);
    let loaded = await this.ctx.storage.get('inner');
    return await loaded.ping(value);
  }

  async directFacetRestoreError(id) {
    let facet = this.ctx.facets.get(`direct-${id}`, () => {
      return {
        class: this.ctx.exports.RestorableFacet({
          props: { prefix: `direct-facet:${id}` },
        }),
      };
    });
    return await facet.makeInner(id);
  }

  async [restore](params) {
    restoreEvents.push(`facet-root:${params.kind}:${params.id}`);
    if (params.kind === 'facet') {
      return this.ctx.facets.get(`restored-${params.id}`, () => {
        return {
          class: this.ctx.exports.RestorableFacet({
            props: { prefix: `facet:${params.id}` },
          }),
        };
      });
    }

    return 123;
  }
}

export class TopRedirectDo extends DurableObject {
  async makeInner(id) {
    return await this.ctx.restore({ kind: 'top-do-inner', id });
  }

  async [restore](params) {
    restoreEvents.push(`top-do:${params.kind}:${params.id}`);
    return this.ctx.exports.StaticTarget({
      props: { prefix: `top-do:${params.id}` },
    });
  }
}

export class NoRestore extends WorkerEntrypoint {
  make() {
    return this.ctx.restore({ kind: 'missing' });
  }
}

// Basic persistent ServiceStub replay from a Dynamic Worker.
export let persistentServiceStubs = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.serviceRoundTrip(1, 'local'),
      'target:local'
    );
    assert.strictEqual(await ctx.exports.Source.restoreCount('service:1'), 1);

    assert.strictEqual(
      await ctx.exports.Source.serviceStorageRoundTrip(2, 'storage'),
      'target:storage'
    );
    assert.strictEqual(await ctx.exports.Source.restoreCount('service:2'), 2);
  },
};

// Persistent RpcStubs can be called, stored, restored, and duplicated.
export let persistentRpcStubs = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.rpcRoundTrip('live', 2),
      'counter:live:2'
    );
    assert.strictEqual(await ctx.exports.Source.restoreCount('rpc:live'), 1);

    assert.strictEqual(
      await ctx.exports.Source.rpcStorageRoundTrip('stored', 5),
      'counter:stored:5'
    );
    assert.strictEqual(await ctx.exports.Source.restoreCount('rpc:stored'), 2);

    assert.strictEqual(
      await ctx.exports.Source.rpcDupStorageRoundTrip('dup', 7),
      'counter:dup:7'
    );
    assert.strictEqual(await ctx.exports.Source.restoreCount('rpc:dup'), 2);
  },
};

// Restore accepts RpcTarget and function results by implicitly wrapping them in RpcStub.
export let implicitPersistentRpcStubs = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.implicitRpcStorageRoundTrip(
        'implicit-rpc-target',
        'target',
        4
      ),
      'counter:target:4'
    );
    assert.strictEqual(
      await ctx.exports.Source.implicitRpcStorageRoundTrip(
        'implicit-rpc-function',
        'function',
        6
      ),
      'counter:function:6'
    );
  },
};

// Facet stubs returned by ctx.restore() can be persisted and replayed.
export let persistentFacetStubs = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    let id = ctx.exports.FacetRoot.idFromName('facet-storage');
    let root = ctx.exports.FacetRoot.get(id);
    assert.strictEqual(
      await root.facetStorageRoundTrip('one', 'stored'),
      'facet:one:stored'
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('facet-root:facet:one'),
      2
    );
  },
};

// Restore chains can be nested through Dynamic Workers and then stored.
export let nestedDynamicRestoreChains = {
  async test(ctrl, env, ctx) {
    assert.strictEqual(
      await ctx.exports.Source.nestedDynamicImmediate('live', 'value'),
      'nested-dynamic:live:value'
    );
    assert.strictEqual(
      await ctx.exports.Source.nestedDynamicStorageRoundTrip('stored', 'value'),
      'nested-dynamic:stored:value'
    );
  },
};

// Restore chains can be nested through facets and then stored.
export let nestedFacetRestoreChains = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    let id = ctx.exports.FacetRoot.idFromName('nested-facet');
    let root = ctx.exports.FacetRoot.get(id);
    assert.strictEqual(
      await root.nestedFacetStorageRoundTrip('nested', 'value'),
      'facet-inner:nested:value'
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('facet-root:facet:nested'),
      2
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('facet:facet-inner:nested'),
      2
    );
  },
};

// Dynamic Workers and facets need to be reached via a restored stub to call ctx.restore().
export let restoreContextErrors = {
  async test(ctrl, env, ctx) {
    await assert.rejects(
      () => ctx.exports.Source.directDynamicRestoreError('no-self'),
      /ctx\.restore\(\) cannot be used in this context/i
    );

    let id = ctx.exports.FacetRoot.idFromName('direct-facet');
    let root = ctx.exports.FacetRoot.get(id);
    await assert.rejects(
      () => root.directFacetRestoreError('no-self'),
      /ctx\.restore\(\) cannot be used in this context/i
    );
  },
};

// Persistent RpcStub channels survive RPC transfer before being stored.
export let rpcTransferPreservesPersistentChannel = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.rpcTransferStorageRoundTrip('transfer', 8),
      'counter:transfer:8'
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('rpc:transfer'),
      2
    );
  },
};

// Dynamic Worker env and props can carry persistent RpcStub channels.
export let dynamicWorkerRpcChannels = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.dynamicEnvRpcRoundTrip('env', 9),
      'counter:env:9'
    );
    assert.strictEqual(
      await ctx.exports.Source.dynamicPropsRpcRoundTrip('props', 10),
      'counter:props:10'
    );
  },
};

// Restore args may themselves contain service and RPC capabilities.
export let restoreArgsWithCapabilities = {
  async test(ctrl, env, ctx) {
    assert.strictEqual(
      await ctx.exports.Source.restoreArgsWithCapabilities('caps', 'value'),
      'arg-service:caps:value|counter:arg-counter-caps:3'
    );
  },
};

// Replay must fail if [restore]() changes between ServiceStub and RpcStub.
export let restoreTypeMismatchErrors = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    await assert.rejects(
      () => ctx.exports.Source.serviceThenRpcMismatch('a'),
      /originally returned a ServiceStub, but on replay it returned an RpcStub/i
    );
    await assert.rejects(
      () => ctx.exports.Source.rpcThenServiceMismatch('b'),
      /originally returned an RpcStub, but on replay it returned a ServiceStub/i
    );
  },
};

// Static Worker restore targets start new restore chains rooted at themselves.
export let staticRestoreTargetStartsNewChain = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.staticRedirectSecurityRoundTrip(
        'worker',
        'value'
      ),
      'static:worker:value'
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('static-redirect:worker'),
      1
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('static:static-inner:worker'),
      2
    );
  },
};

// Top-level Durable Object restore targets also start new chains rooted at themselves.
export let topLevelDoRestoreTargetStartsNewChain = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    assert.strictEqual(
      await ctx.exports.Source.topLevelDoRedirectSecurityRoundTrip(
        'actor',
        'value'
      ),
      'top-do:actor:value'
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('top-do-redirect:actor'),
      1
    );
    assert.strictEqual(
      await ctx.exports.Source.restoreCount('top-do:top-do-inner:actor'),
      2
    );
  },
};

// Restore reports invalid return values, thrown errors, missing methods, and plain RpcStub storage.
export let restoreErrors = {
  async test(ctrl, env, ctx) {
    assert.strictEqual(typeof ctx.restore, 'function');

    await assert.rejects(
      () => ctx.exports.Source.makeBad(4),
      /\[restore\]\(\) method must return a ServiceStub or RpcStub/i
    );
    await assert.rejects(
      () => ctx.restore({ kind: 'throws', id: 4 }),
      /restore failed/
    );
    await assert.rejects(
      () => ctx.exports.NoRestore.make(),
      /does not implement a \[restore\]\(\) method/i
    );
    await assert.rejects(
      () => ctx.exports.Source.plainRpcStorageError('plain'),
      /RpcStub cannot be serialized in this context/i
    );
  },

  async [restore](params) {
    throw new Error('restore failed');
  },
};

// A root Durable Object can use ctx.restore() even when there is no client waiting for a response
// (here, from a waitUntil() task that runs while the actor is draining). This is a regression test
// for a bug where the actor's self-channel was dropped during drain() to avoid an eviction cycle.
export let actorRestoreWithNoClientWaiting = {
  async test(ctrl, env, ctx) {
    restoreEvents = [];

    let id = ctx.exports.Store.idFromName('wait-until-restore');
    let store = ctx.exports.Store.get(id);

    assert.strictEqual(await store.startRestoreInWaitUntil('wu', 5), 'started');

    // Wait (without keeping any request to the actor active) for the waitUntil task to run its
    // ctx.restore() while no client is waiting.
    let result;
    for (let i = 0; i < 50; i++) {
      await new Promise((resolve) => setTimeout(resolve, 20));
      result = await store.getWaitUntilResult();
      if (result !== undefined) break;
    }

    assert.strictEqual(result, 'counter:wu:5');
  },
};
