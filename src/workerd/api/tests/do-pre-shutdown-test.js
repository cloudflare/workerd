// Tests for the preShutdown() Durable Object lifecycle hook (dev parity): the hook runs on
// workerd's graceful eviction paths, before teardown, with its storage writes durably flushed
// before a successor instance can observe storage. Uses the test-only unsafe.evict() to trigger
// eviction without waiting out the 10-second inactivity timer.

import assert from 'node:assert';
import unsafe from 'workerd:unsafe';
import { DurableObject } from 'cloudflare:workers';

// Tracks activity in memory only; checkpoints to storage from the preShutdown hook. A successor
// instance must observe the checkpoint.
export class HookObject extends DurableObject {
  #bumps = 0;

  async bump() {
    return ++this.#bumps;
  }

  async getCheckpoint() {
    return (await this.ctx.storage.get('checkpoint')) ?? null;
  }

  async preShutdown(info) {
    // Fire-and-forget put: the sync fast path. The runtime drains this write before teardown.
    this.ctx.storage.put('checkpoint', {
      bumps: this.#bumps,
      reason: info.reason,
    });
  }
}

// The hook returns a promise; the runtime must await it.
export class AsyncHookObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async getCheckpoint() {
    return (await this.ctx.storage.get('checkpoint')) ?? null;
  }

  async preShutdown(info) {
    await scheduler.wait(50);
    await this.ctx.storage.put('checkpoint', { reason: info.reason });
  }
}

// A throwing hook must not prevent eviction, and writes issued before the throw must still be
// drained.
export class ThrowingHookObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async getMarker() {
    return (await this.ctx.storage.get('marker')) ?? null;
  }

  async preShutdown() {
    this.ctx.storage.put('marker', 'wrote-before-throw');
    throw new Error('preShutdown handler failure (expected by test)');
  }
}

// No hook defined: eviction must proceed as before.
export class NoHookObject extends DurableObject {
  #bumps = 0;

  async bump() {
    return ++this.#bumps;
  }
}

// The flagship agent pattern: the hook schedules an alarm, and the alarm later resurrects the
// actor.
export class AlarmHookObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async getAlarmRan() {
    return (await this.ctx.storage.get('alarmRan')) ?? false;
  }

  async preShutdown() {
    await this.ctx.storage.setAlarm(Date.now() + 100);
  }

  async alarm() {
    await this.ctx.storage.put('alarmRan', true);
  }
}

export default {
  async test(ctrl, env, ctx) {
    // Checkpoint written by the hook is visible to the successor, with reason "inactive".
    {
      const stub = env.HOOK.get(env.HOOK.idFromName('a'));
      assert.strictEqual(await stub.bump(), 1);
      assert.strictEqual(await stub.bump(), 2);
      assert.strictEqual(await stub.bump(), 3);
      await unsafe.evict(stub);
      assert.deepStrictEqual(await stub.getCheckpoint(), {
        bumps: 3,
        reason: 'inactive',
      });
      // The successor is a fresh instance (in-memory state is gone).
      assert.strictEqual(await stub.bump(), 1);
    }

    // An async hook is awaited to completion before teardown.
    {
      const stub = env.ASYNC_HOOK.get(env.ASYNC_HOOK.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub);
      assert.deepStrictEqual(await stub.getCheckpoint(), {
        reason: 'inactive',
      });
    }

    // A throwing hook doesn't block eviction; writes issued before the throw are drained.
    {
      const stub = env.THROWING_HOOK.get(env.THROWING_HOOK.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub);
      assert.strictEqual(await stub.getMarker(), 'wrote-before-throw');
    }

    // A class without the method evicts as before.
    {
      const stub = env.NO_HOOK.get(env.NO_HOOK.idFromName('a'));
      assert.strictEqual(await stub.bump(), 1);
      await unsafe.evict(stub);
      assert.strictEqual(await stub.bump(), 1);
    }

    // The hook can setAlarm(); the alarm later fires and resurrects the actor.
    {
      const stub = env.ALARM_HOOK.get(env.ALARM_HOOK.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub);
      // The alarm was scheduled for +100ms. Sleep well past it to stay robust under CI load
      // (there is no way to be notified of the alarm without touching the actor).
      await scheduler.wait(2000);
      assert.strictEqual(await stub.getAlarmRan(), true);
    }

    // A worker without the compat flag: handler present but never invoked.
    {
      const stub = env.FLAG_OFF.get(env.FLAG_OFF.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub);
      assert.strictEqual(await stub.getHookRan(), false);
    }
  },
};
