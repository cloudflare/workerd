// Tests for the onEvict Durable Object lifecycle hook (dev parity): the hook runs on
// workerd's graceful eviction paths, before teardown, with its storage writes durably flushed
// before a successor instance can observe storage. Uses the test-only unsafe.evict() to trigger
// eviction without waiting out the 10-second inactivity timer.

import assert from 'node:assert';
import unsafe from 'workerd:unsafe';
import { DurableObject, onEvict } from 'cloudflare:workers';

assert.strictEqual(typeof onEvict, 'symbol');
assert.notStrictEqual(onEvict, Symbol.for('cloudflare:workers:onEvict'));

// Tracks activity in memory only; checkpoints to storage from the onEvict hook. A successor
// instance must observe the checkpoint.
export class HookObject extends DurableObject {
  #bumps = 0;

  async bump() {
    return ++this.#bumps;
  }

  async getCheckpoint() {
    return (await this.ctx.storage.get('checkpoint')) ?? null;
  }

  async [onEvict](info) {
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

  async [onEvict](info) {
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

  async [onEvict]() {
    this.ctx.storage.put('marker', 'wrote-before-throw');
    throw new Error('onEvict handler failure (expected by test)');
  }
}

// No hook defined: eviction must proceed as before.
export class NoHookObject extends DurableObject {
  #bumps = 0;

  async bump() {
    return ++this.#bumps;
  }
}

// Used by the racing-request test below: the hook advertises that it has started via a shared
// module global (test and object run in the same isolate), then stalls so the test can get a
// request in flight before the hook finishes.
export class RacingHookObject extends DurableObject {
  #bumps = 0;

  async bump() {
    return ++this.#bumps;
  }

  async [onEvict]() {
    globalThis.racingHookStarted = true;
    await scheduler.wait(500);
  }
}

// Hard-aborts its own actor from inside the hook. Used to verify that a hard abort racing the
// hook (which drops the container's owning reference to the actor) is memory-safe: the hook is
// cut short, teardown settles, and the object rebuilds on next use.
export class AbortingHookObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async [onEvict]() {
    this.ctx.abort(new Error('aborted from onEvict (expected by test)'));
  }
}

// Leaves background work behind when the hook returns: a pending setTimeout() and a waitUntil().
// Returning from the hook means no more code may run in this object, so neither may fire; the
// runtime cancels them rather than letting them delay (or resume after) the teardown. The
// callbacks record in module globals (test and object share an isolate) so the test can assert
// they never ran.
export class LeftoverWorkObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async [onEvict]() {
    globalThis.leftoverTimerRan = false;
    globalThis.leftoverWaitUntilRan = false;
    setTimeout(() => {
      globalThis.leftoverTimerRan = true;
    }, 100);
    this.ctx.waitUntil(
      scheduler.wait(100).then(() => {
        globalThis.leftoverWaitUntilRan = true;
      })
    );
  }
}

// Root/facet pair used to verify that only the root actor gets the hook (v1 excludes facets).
// The hooks record that they ran in module-level globals, which the test can read because it
// runs in the same isolate as the objects.
export class FacetHookChild extends DurableObject {
  async ping() {
    return 'pong';
  }

  async [onEvict]() {
    // Must never run: facets don't get the hook in v1.
    globalThis.facetHookRan = true;
  }
}

export class FacetHookRoot extends DurableObject {
  async prime() {
    const child = this.ctx.facets.get('child', () => ({
      class: this.ctx.exports.FacetHookChild({}),
    }));
    return await child.ping();
  }

  async [onEvict]() {
    globalThis.rootHookRan = true;
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

  async [onEvict]() {
    await this.ctx.storage.setAlarm(Date.now() + 100);
  }

  async alarm() {
    await this.ctx.storage.put('alarmRan', true);
  }
}

class InheritedHookObject extends DurableObject {
  async [onEvict](info) {
    await this.ctx.storage.put('symbol-result', {
      correctThis: this instanceof SymbolHookObject,
      reason: info.reason,
      source: 'captured',
    });
  }
}

export class SymbolHookObject extends InheritedHookObject {
  async callHookDirectly() {
    await this[onEvict]({ reason: 'direct' });
    return this.ctx.storage.get('symbol-result');
  }

  async replaceHook() {
    this[onEvict] = async () => {
      await this.ctx.storage.put('symbol-result', { source: 'replacement' });
    };
  }

  async getSymbolResult() {
    return this.ctx.storage.get('symbol-result');
  }

  async preShutdown() {
    return 'ordinary preShutdown RPC method';
  }

  async onEvict() {
    return 'ordinary onEvict RPC method';
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

    // Destruction is committed once the hook starts, matching production: a request that
    // arrives while the hook is running does not revive the doomed actor (nor cancel the
    // eviction); it waits for the teardown to settle and is then served by a fresh instance.
    {
      const stub = env.RACING_HOOK.get(env.RACING_HOOK.idFromName('a'));
      assert.strictEqual(await stub.bump(), 1);
      globalThis.racingHookStarted = false;
      const evictPromise = unsafe.evict(stub);
      while (!globalThis.racingHookStarted) {
        await scheduler.wait(10);
      }
      // The hook is now stalling. A request sent mid-hook must be served by the successor
      // instance (whose bump counter starts over), not by the instance being torn down.
      const racingBump = stub.bump();
      await evictPromise;
      assert.strictEqual(await racingBump, 1);
    }

    // Background work the hook leaves behind (a pending setTimeout(), a waitUntil()) is
    // canceled when the hook returns: the end of the hook means no more code runs in the
    // object, so neither callback may ever fire.
    {
      const stub = env.LEFTOVER_WORK.get(env.LEFTOVER_WORK.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub);
      // Wait well past the leftover work's 100ms delay before asserting it never ran.
      await scheduler.wait(500);
      assert.strictEqual(globalThis.leftoverTimerRan, false);
      assert.strictEqual(globalThis.leftoverWaitUntilRan, false);
    }

    // Symbol lookup includes inherited methods, binds the actor as `this`, and captures the
    // function at construction time. String methods with similar names remain ordinary RPC.
    {
      const stub = env.SYMBOL_HOOK.get(env.SYMBOL_HOOK.idFromName('a'));
      assert.strictEqual(
        await stub.preShutdown(),
        'ordinary preShutdown RPC method'
      );
      assert.strictEqual(await stub.onEvict(), 'ordinary onEvict RPC method');
      assert.deepStrictEqual(await stub.callHookDirectly(), {
        correctThis: true,
        reason: 'direct',
        source: 'captured',
      });
      await stub.replaceHook();
      await unsafe.evict(stub);
      assert.deepStrictEqual(await stub.getSymbolResult(), {
        correctThis: true,
        reason: 'inactive',
        source: 'captured',
      });
    }

    // A hook that hard-aborts its own actor: the abort drops the container's owning reference
    // mid-hook, which must be memory-safe (the hook pins the actor for its own duration). The
    // eviction settles -- possibly with an error, since the actor is now broken -- and the
    // object rebuilds on next use.
    {
      const stub = env.ABORTING_HOOK.get(env.ABORTING_HOOK.idFromName('a'));
      assert.strictEqual(await stub.ping(), 'pong');
      await unsafe.evict(stub).catch(() => {});
      const stub2 = env.ABORTING_HOOK.get(env.ABORTING_HOOK.idFromName('a'));
      assert.strictEqual(await stub2.ping(), 'pong');
    }

    // Only the root actor's hook runs when an actor tree is torn down; its facets' hooks do
    // not (v1 excludes facets). evictAllDurableObjects() is the only test-only eviction that
    // walks facet trees. This runs last so that evicting every other running actor doesn't
    // disturb the earlier cases.
    {
      const stub = env.FACET_HOOK_ROOT.get(env.FACET_HOOK_ROOT.idFromName('a'));
      assert.strictEqual(await stub.prime(), 'pong');
      globalThis.rootHookRan = false;
      globalThis.facetHookRan = false;
      await unsafe.evictAllDurableObjects();
      assert.strictEqual(globalThis.rootHookRan, true);
      assert.strictEqual(globalThis.facetHookRan, false);
    }
  },
};
