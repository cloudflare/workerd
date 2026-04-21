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
