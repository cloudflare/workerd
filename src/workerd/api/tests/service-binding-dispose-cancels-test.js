// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { scheduler } from 'node:timers/promises';

// Read from the runtime rather than a binding, so the all-compat-flags variant stays correct.
const CANCELS =
  globalThis.Cloudflare.compatibilityFlags['rpc_dispose_cancels_call'];

// How long the callee's handlers wait before they finish.
const HANG_MS = 100;

// The state of each event, by the ID the caller passed as its cron or queue name.
const calls = new Map();

async function hang(id) {
  calls.set(id, 'started');
  await scheduler.wait(HANG_MS);
  calls.set(id, 'finished');
}

export class Callee extends WorkerEntrypoint {
  async scheduled(controller) {
    await hang(controller.cron);
  }

  async queue(batch) {
    await hang(batch.queue);
    batch.ackAll();
  }
}

// Sends each kind of event to the callee, which shares this isolate.
const SEND = {
  scheduled: (env, id) => env.SELF.scheduled({ cron: id }),
  queue: (env, id) =>
    env.SELF.queue(id, [
      { id: '1', timestamp: new Date(), body: 'message', attempts: 1 },
    ]),
};

// Sends an event and waits until the callee's handler is running. The event's promise is wrapped
// in an object because returning it from an async function would await it.
async function startEvent(env, kind, id) {
  const event = SEND[kind](env, id);
  while (calls.get(id) !== 'started') {
    await scheduler.wait(1);
  }
  return { event };
}

function isAbortError(kind) {
  return (error) => {
    assert.strictEqual(
      `${error.name}: ${error.message}`,
      `AbortError: The ${kind}() call was canceled because its promise was disposed.`
    );
    return true;
  };
}

function disposeCancelsEvent(kind) {
  return {
    async test(_controller, env) {
      const id = `${kind}-dispose`;
      const { event } = await startEvent(env, kind, id);
      assert.ok(event instanceof Promise);

      if (CANCELS) {
        event[Symbol.dispose]();
        // Disposing again, before or after the promise rejects, does nothing more.
        event[Symbol.dispose]();
        await assert.rejects(event, isAbortError(kind));
        event[Symbol.dispose]();
        await assert.rejects(event, isAbortError(kind));
        // The canceled event ended the callee's request, so its timer never fires.
        await scheduler.wait(HANG_MS * 3);
        assert.strictEqual(calls.get(id), 'started');
      } else {
        assert.strictEqual(event[Symbol.dispose], undefined);
        assert.strictEqual((await event).outcome, 'ok');
        assert.strictEqual(calls.get(id), 'finished');
      }
    },
  };
}

function usingCancelsWithoutUnhandledRejection(kind) {
  return {
    async test(_controller, env) {
      // Without the flag, the promise isn't disposable, so `using` would throw.
      if (!CANCELS) return;
      const id = `${kind}-using`;
      const events = [];
      const handler = (event) => {
        events.push(event);
        event.preventDefault();
      };
      addEventListener('unhandledrejection', handler);
      try {
        {
          using _event = (await startEvent(env, kind, id)).event;
        }
        await scheduler.wait(HANG_MS * 3);
        assert.strictEqual(events.length, 0);
        assert.strictEqual(calls.get(id), 'started');
      } finally {
        removeEventListener('unhandledrejection', handler);
      }
    },
  };
}

// Once the event has finished, disposing its promise does nothing.
function disposeAfterCompletionDoesNothing(kind) {
  return {
    async test(_controller, env) {
      if (!CANCELS) return;
      const event = SEND[kind](env, `${kind}-after`);
      const result = await event;
      event[Symbol.dispose]();
      assert.strictEqual(result.outcome, 'ok');
      assert.strictEqual(await event, result);
    },
  };
}

export const disposeCancelsScheduled = disposeCancelsEvent('scheduled');
export const disposeCancelsQueue = disposeCancelsEvent('queue');
export const usingCancelsScheduledWithoutUnhandledRejection =
  usingCancelsWithoutUnhandledRejection('scheduled');
export const usingCancelsQueueWithoutUnhandledRejection =
  usingCancelsWithoutUnhandledRejection('queue');
export const disposeAfterScheduledCompletesDoesNothing =
  disposeAfterCompletionDoesNothing('scheduled');
export const disposeAfterQueueCompletesDoesNothing =
  disposeAfterCompletionDoesNothing('queue');
