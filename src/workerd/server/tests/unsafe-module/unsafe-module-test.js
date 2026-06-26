// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import unsafe from 'workerd:unsafe';
import { DurableObject } from 'cloudflare:workers';

function createTestObject(type) {
  return class {
    id = crypto.randomUUID();
    fetch() {
      return new Response(`${type}:${this.id}`);
    }
  };
}
export const TestDurableObject = createTestObject('durable');
export const TestDurableObjectPreventEviction = createTestObject(
  'durable-prevent-eviction'
);
export const TestEphemeralObject = createTestObject('ephemeral');
export const TestEphemeralObjectPreventEviction = createTestObject(
  'ephemeral-prevent-eviction'
);

// DO with persistent storage for verifying deleteAllDurableObjects() wipes data.
export class StorageObject extends DurableObject {
  async getValue() {
    return (await this.ctx.storage.get('key')) ?? null;
  }
  async setValue(value) {
    await this.ctx.storage.put('key', value);
  }
}

// DO that tracks both in-memory and persisted state, used to verify that evict() tears down the
// instance (losing in-memory state) while preserving durable storage.
export class CounterObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.inMemory = 0;
  }
  async bump() {
    this.inMemory++;
    const stored = ((await this.ctx.storage.get('count')) ?? 0) + 1;
    await this.ctx.storage.put('count', stored);
    return { inMemory: this.inMemory, stored };
  }
  async getState() {
    return {
      inMemory: this.inMemory,
      stored: (await this.ctx.storage.get('count')) ?? 0,
    };
  }
  async slowBump() {
    await new Promise((resolve) => setTimeout(resolve, 10));
    return await this.bump();
  }
}

// Same as CounterObject, but its namespace sets preventEviction.
export class CounterObjectPreventEviction extends CounterObject {}

// Root/facet pair used to verify evictAllDurableObjects() walks the whole running actor tree.
export class FacetCounterObject extends CounterObject {}

export class FacetRootObject extends CounterObject {
  getFacet() {
    return this.ctx.facets.get('child', () => ({
      class: this.ctx.exports.FacetCounterObject({}),
    }));
  }
  async bumpBoth() {
    return {
      root: await this.bump(),
      facet: await this.getFacet().bump(),
    };
  }
  async getBothState() {
    return {
      root: await this.getState(),
      facet: await this.getFacet().getState(),
    };
  }
}

// DO that accepts a hibernatable WebSocket. Used to verify that evict() hibernates the socket
// (keeping the connection alive) and rebuilds the instance on the next event.
export class HibernationObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.wokeCount = 0; // in-memory; resets when the instance is rebuilt.
  }
  async fetch(request) {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }
  async webSocketMessage(ws, message) {
    if (message === 'bye') {
      ws.close(1000, 'bye');
      return;
    }
    this.wokeCount++;
    const stored = ((await this.ctx.storage.get('count')) ?? 0) + 1;
    await this.ctx.storage.put('count', stored);
    ws.send(JSON.stringify({ wokeCount: this.wokeCount, stored }));
  }
  async getStored() {
    return (await this.ctx.storage.get('count')) ?? 0;
  }
}

function nextMessage(ws) {
  return new Promise((resolve, reject) => {
    ws.addEventListener('message', (event) => resolve(event.data), {
      once: true,
    });
    ws.addEventListener('error', reject, { once: true });
  });
}

function nextClose(ws) {
  return new Promise((resolve, reject) => {
    ws.addEventListener('close', () => resolve(), { once: true });
    ws.addEventListener('error', reject, { once: true });
  });
}

// DO with an alarm for verifying deleteAllDurableObjects() cancels alarms.
let alarmTriggers = 0;
export class AlarmObject extends DurableObject {
  get scheduledTime() {
    return this.ctx.storage.getAlarm();
  }
  async scheduleIn(delay) {
    await this.ctx.storage.setAlarm(Date.now() + delay);
  }
  alarm() {
    alarmTriggers++;
  }
}

export const test_abort_all_durable_objects = {
  async test(ctrl, env, ctx) {
    const durableId = env.DURABLE.newUniqueId();
    const durablePreventEvictionId = env.DURABLE_PREVENT_EVICTION.newUniqueId();

    let durableStub = env.DURABLE.get(durableId);
    const durablePreventEvictionStub = env.DURABLE_PREVENT_EVICTION.get(
      durablePreventEvictionId
    );
    let ephemeralStub = env.EPHEMERAL.get('thing');
    const ephemeralPreventEvictionStub =
      env.EPHEMERAL_PREVENT_EVICTION.get('thing');

    const durableRes1 = await (await durableStub.fetch('http://x')).text();
    const durablePreventEvictionRes1 = await (
      await durablePreventEvictionStub.fetch('http://x')
    ).text();
    const ephemeralRes1 = await (await ephemeralStub.fetch('http://x')).text();
    const ephemeralPreventEvictionRes1 = await (
      await ephemeralPreventEvictionStub.fetch('http://x')
    ).text();

    await unsafe.abortAllDurableObjects();

    // Since the objects were aborted, trying to use their stubs now should reject.
    await assert.rejects(() => durableStub.fetch('http://x'), {
      name: 'Error',
      message: 'Application called abortAllDurableObjects().',
    });
    await assert.rejects(() => ephemeralStub.fetch('http://x'), {
      name: 'Error',
      message: 'Application called abortAllDurableObjects().',
    });

    // Recreate the stubs because they are broken.
    durableStub = env.DURABLE.get(durableId);
    ephemeralStub = env.EPHEMERAL.get('thing');

    const durableRes2 = await (await durableStub.fetch('http://x')).text();
    const durablePreventEvictionRes2 = await (
      await durablePreventEvictionStub.fetch('http://x')
    ).text();
    const ephemeralRes2 = await (await ephemeralStub.fetch('http://x')).text();
    const ephemeralPreventEvictionRes2 = await (
      await ephemeralPreventEvictionStub.fetch('http://x')
    ).text();

    // Irrespective of abort status, verify responses start with expected prefix
    assert.match(durableRes1, /^durable:/);
    assert.match(durableRes2, /^durable:/);
    assert.match(durablePreventEvictionRes1, /^durable-prevent-eviction:/);
    assert.match(ephemeralRes1, /^ephemeral:/);
    assert.match(ephemeralRes2, /^ephemeral:/);
    assert.match(ephemeralPreventEvictionRes1, /^ephemeral-prevent-eviction:/);

    // Response from aborted objects should change
    assert.notStrictEqual(durableRes1, durableRes2);
    assert.notStrictEqual(ephemeralRes1, ephemeralRes2);

    // Response from objects in namespaces that have prevent eviction set shouldn't change
    assert.strictEqual(durablePreventEvictionRes1, durablePreventEvictionRes2);
    assert.strictEqual(
      ephemeralPreventEvictionRes1,
      ephemeralPreventEvictionRes2
    );
  },
};

export const test_delete_all_durable_objects = {
  async test(ctrl, env, ctx) {
    // Write some data to a durable object.
    const id = env.STORAGE.idFromName('test-delete');
    let stub = env.STORAGE.get(id);
    await stub.setValue('hello');
    assert.strictEqual(await stub.getValue(), 'hello');

    // Delete all durable objects.
    await unsafe.deleteAllDurableObjects();

    // Old stub should be broken.
    await assert.rejects(() => stub.getValue(), {
      name: 'Error',
      message: 'Application called deleteAllDurableObjects().',
    });

    // Recreate the stub — storage should be gone (files were deleted from disk).
    stub = env.STORAGE.get(id);
    assert.strictEqual(await stub.getValue(), null);
  },
};

export const test_delete_all_durable_objects_alarms = {
  async test(ctrl, env, ctx) {
    const id = env.ALARM.newUniqueId();
    const stub = env.ALARM.get(id);

    const alarmsBefore = alarmTriggers;
    await stub.scheduleIn(500);
    assert.notStrictEqual(await stub.scheduledTime, null);

    // Delete everything — alarms should be cancelled and not fire.
    await unsafe.deleteAllDurableObjects();
    await scheduler.wait(1000);
    assert.strictEqual(alarmTriggers, alarmsBefore); // alarm did not fire
  },
};

export const test_delete_all_durable_objects_respects_prevent_eviction = {
  async test(ctrl, env, ctx) {
    const id = env.DURABLE_PREVENT_EVICTION.newUniqueId();
    const stub = env.DURABLE_PREVENT_EVICTION.get(id);
    const res1 = await (await stub.fetch('http://x')).text();

    await unsafe.deleteAllDurableObjects();

    // preventEviction namespace should be untouched — same response (same instance).
    const res2 = await (await stub.fetch('http://x')).text();
    assert.strictEqual(res1, res2);
  },
};

export const test_evict = {
  async test(ctrl, env, ctx) {
    const id = env.COUNTER.idFromName('evict');
    const stub = env.COUNTER.get(id);

    assert.deepStrictEqual(await stub.bump(), { inMemory: 1, stored: 1 });
    assert.deepStrictEqual(await stub.bump(), { inMemory: 2, stored: 2 });

    // Gracefully evict the instance from the isolate.
    await unsafe.evict(stub);

    // The stub keeps working (no abort), but the instance was rebuilt: in-memory state is gone
    // while durable storage survived.
    assert.deepStrictEqual(await stub.getState(), { inMemory: 0, stored: 2 });
  },
};

export const test_evict_waits_for_in_flight_request = {
  async test(ctrl, env, ctx) {
    const stub = env.COUNTER.get(env.COUNTER.idFromName('evict-in-flight'));

    const pending = stub.slowBump();
    await unsafe.evict(stub);

    assert.deepStrictEqual(await pending, { inMemory: 1, stored: 1 });
    assert.deepStrictEqual(await stub.getState(), { inMemory: 0, stored: 1 });
  },
};

export const test_evict_concurrent_calls = {
  async test(ctrl, env, ctx) {
    const stub = env.COUNTER.get(env.COUNTER.idFromName('evict-concurrent'));
    await stub.bump();

    await Promise.all([unsafe.evict(stub), unsafe.evict(stub)]);
    assert.deepStrictEqual(await stub.getState(), { inMemory: 0, stored: 1 });
  },
};

export const test_evict_respects_prevent_eviction = {
  async test(ctrl, env, ctx) {
    const stub = env.COUNTER_PREVENT_EVICTION.get(
      env.COUNTER_PREVENT_EVICTION.idFromName('evict-prevent')
    );
    await stub.bump();

    await assert.rejects(() => unsafe.evict(stub), {
      name: 'Error',
      message:
        'Cannot evict Durable Object: its namespace has preventEviction set.',
    });

    assert.deepStrictEqual(await stub.getState(), { inMemory: 1, stored: 1 });
  },
};

export const test_evict_not_running = {
  async test(ctrl, env, ctx) {
    // Never sent a request, so the DO was never instantiated.
    const stub = env.COUNTER.get(env.COUNTER.idFromName('never-running'));
    await assert.rejects(() => unsafe.evict(stub), {
      name: 'Error',
      message: 'Cannot evict Durable Object: it is not currently running.',
    });
  },
};

export const test_evict_non_durable_object = {
  async test(ctrl, env, ctx) {
    // SELF_SERVICE is an ordinary service binding, not a Durable Object stub.
    await assert.rejects(() => unsafe.evict(env.SELF_SERVICE), {
      name: 'Error',
      message: 'evict() can only be used on a Durable Object stub.',
    });
  },
};

export const test_evict_rejects_invalid_websocket_mode = {
  async test(ctrl, env, ctx) {
    const stub = env.COUNTER.get(env.COUNTER.idFromName('evict-invalid-mode'));
    await stub.bump();

    await assert.rejects(() => unsafe.evict(stub, { webSockets: 'explode' }), {
      name: 'TypeError',
      message: 'options.webSockets must be "hibernate" or "close".',
    });
    await assert.rejects(
      () => unsafe.evictAllDurableObjects({ webSockets: 'explode' }),
      {
        name: 'TypeError',
        message: 'options.webSockets must be "hibernate" or "close".',
      }
    );

    assert.deepStrictEqual(await stub.getState(), { inMemory: 1, stored: 1 });
  },
};

export const test_evict_all_durable_objects = {
  async test(ctrl, env, ctx) {
    const a = env.COUNTER.get(env.COUNTER.idFromName('all-a'));
    const b = env.COUNTER.get(env.COUNTER.idFromName('all-b'));
    const prevent = env.COUNTER_PREVENT_EVICTION.get(
      env.COUNTER_PREVENT_EVICTION.idFromName('all-prevent')
    );

    await a.bump();
    await a.bump();
    await b.bump();
    await prevent.bump();

    await unsafe.evictAllDurableObjects();

    // Evictable counters were rebuilt (in-memory reset), storage preserved.
    assert.deepStrictEqual(await a.getState(), { inMemory: 0, stored: 2 });
    assert.deepStrictEqual(await b.getState(), { inMemory: 0, stored: 1 });

    // preventEviction namespace was skipped — in-memory state survives.
    assert.deepStrictEqual(await prevent.getState(), {
      inMemory: 1,
      stored: 1,
    });
  },
};

export const test_evict_all_durable_objects_evicts_facets = {
  async test(ctrl, env, ctx) {
    const root = env.FACET_ROOT.get(
      env.FACET_ROOT.idFromName('evict-all-facets')
    );

    assert.deepStrictEqual(await root.bumpBoth(), {
      root: { inMemory: 1, stored: 1 },
      facet: { inMemory: 1, stored: 1 },
    });
    assert.deepStrictEqual(await root.bumpBoth(), {
      root: { inMemory: 2, stored: 2 },
      facet: { inMemory: 2, stored: 2 },
    });

    await unsafe.evictAllDurableObjects();

    // Both the root and facet instances were rebuilt, but both durable stores survived.
    assert.deepStrictEqual(await root.getBothState(), {
      root: { inMemory: 0, stored: 2 },
      facet: { inMemory: 0, stored: 2 },
    });
  },
};

export const test_evict_hibernates_websockets = {
  async test(ctrl, env, ctx) {
    const stub = env.HIBERNATE.get(env.HIBERNATE.idFromName('room'));
    const res = await stub.fetch('http://x', {
      headers: { Upgrade: 'websocket' },
    });
    const ws = res.webSocket;
    assert(ws, 'expected a WebSocket in the response');
    ws.accept();

    // Fresh instance: wokeCount 0 -> 1, stored 0 -> 1.
    let msg = nextMessage(ws);
    ws.send('a');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 1 });

    // Evict: hibernates the socket (connection stays open) and tears down the instance.
    await unsafe.evict(stub);

    // The same socket still delivers (proving it hibernated rather than closing), and the event
    // is handled by a rebuilt instance: wokeCount resets to 1, but storage survived (stored -> 2).
    msg = nextMessage(ws);
    ws.send('b');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 2 });

    // Tell the DO to close the socket so the test tears down cleanly.
    const closed = new Promise((resolve) =>
      ws.addEventListener('close', () => resolve(), { once: true })
    );
    ws.send('bye');
    await closed;
  },
};

export const test_evict_closes_websockets = {
  async test(ctrl, env, ctx) {
    const stub = env.HIBERNATE.get(env.HIBERNATE.idFromName('close-room'));
    let res = await stub.fetch('http://x', {
      headers: { Upgrade: 'websocket' },
    });
    let ws = res.webSocket;
    assert(ws, 'expected a WebSocket in the response');
    ws.accept();

    let msg = nextMessage(ws);
    ws.send('a');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 1 });

    const closed = nextClose(ws);
    await unsafe.evict(stub, { webSockets: 'close' });
    await closed;

    // Storage survived, but the closed socket cannot deliver the wakeup event. Open a new socket to
    // verify the instance was rebuilt and storage continues from the previous value.
    assert.strictEqual(await stub.getStored(), 1);
    res = await stub.fetch('http://x', {
      headers: { Upgrade: 'websocket' },
    });
    ws = res.webSocket;
    assert(ws, 'expected a WebSocket in the response');
    ws.accept();

    msg = nextMessage(ws);
    ws.send('b');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 2 });

    const cleanupClosed = nextClose(ws);
    ws.send('bye');
    await cleanupClosed;
  },
};

export const test_evict_all_durable_objects_closes_websockets = {
  async test(ctrl, env, ctx) {
    const stub = env.HIBERNATE.get(env.HIBERNATE.idFromName('close-all-room'));
    let res = await stub.fetch('http://x', {
      headers: { Upgrade: 'websocket' },
    });
    let ws = res.webSocket;
    assert(ws, 'expected a WebSocket in the response');
    ws.accept();

    let msg = nextMessage(ws);
    ws.send('a');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 1 });

    const closed = nextClose(ws);
    await unsafe.evictAllDurableObjects({ webSockets: 'close' });
    await closed;

    assert.strictEqual(await stub.getStored(), 1);
    res = await stub.fetch('http://x', {
      headers: { Upgrade: 'websocket' },
    });
    ws = res.webSocket;
    assert(ws, 'expected a WebSocket in the response');
    ws.accept();

    msg = nextMessage(ws);
    ws.send('b');
    assert.deepStrictEqual(JSON.parse(await msg), { wokeCount: 1, stored: 2 });

    const cleanupClosed = nextClose(ws);
    ws.send('bye');
    await cleanupClosed;
  },
};
