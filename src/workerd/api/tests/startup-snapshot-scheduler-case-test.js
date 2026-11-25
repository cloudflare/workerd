// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-scheduler-test.js: MODE says whether the top level reads `scheduler`.

import { env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

function caught(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

// What a call of wait() throws or its promise rejects with, or undefined: without
// capture_async_api_throws a failure is thrown.
async function failure(call) {
  try {
    await call();
  } catch (error) {
    return error;
  }
  return undefined;
}

// What the top level keeps in 'kept': the object, its wait(), its constructor, and what wait()
// does outside of a request, where it cannot set a timer and where a wrong argument or receiver
// fails as in one.
let kept = null;
if (MODE === 'kept') {
  const { wait } = globalThis.scheduler;
  kept = {
    scheduler: globalThis.scheduler,
    wait,
    Constructor: globalThis.scheduler.constructor,
    outsideOfARequest: failure(() => globalThis.scheduler.wait(1)),
    noDelay: failure(() => globalThis.scheduler.wait()),
    badReceiver: caught(() => wait(1)),
  };
}

// What holds of the object in every case.
async function commonChecks() {
  const Constructor = scheduler.constructor;
  const controller = new AbortController();
  const aborted = failure(() =>
    scheduler.wait(10_000, { signal: controller.signal })
  );
  controller.abort(new RangeError('aborted'));
  const abortedWith = await aborted;
  const waited = scheduler.wait(1);
  const noDelay = await failure(() => scheduler.wait());
  const badReceiver = caught(() => scheduler.wait.call({}, 1));
  return {
    sameObjectEveryTime: scheduler === globalThis.scheduler,
    ownKeys: Reflect.ownKeys(scheduler).length === 0,
    isScheduler:
      scheduler instanceof Constructor &&
      Object.getPrototypeOf(scheduler) === Constructor.prototype &&
      Constructor.name === 'Scheduler',
    notConstructible:
      caught(() => new Constructor())?.message === 'Illegal constructor',
    notSerializable:
      caught(() => structuredClone(scheduler))?.name === 'DataCloneError',
    waits: (await waited) === undefined,
    // What wait() makes and throws belongs to this Worker's realm.
    promiseIsOfThisRealm: Object.getPrototypeOf(waited) === Promise.prototype,
    abortedIsOfThisRealm:
      Object.getPrototypeOf(abortedWith) === RangeError.prototype &&
      abortedWith.message === 'aborted',
    noDelayIsOfThisRealm:
      Object.getPrototypeOf(noDelay) === TypeError.prototype,
    badReceiverIsOfThisRealm:
      Object.getPrototypeOf(badReceiver) === TypeError.prototype,
  };
}

async function checks() {
  const common = await commonChecks();
  if (kept === null) return common;
  const outsideOfARequest = await kept.outsideOfARequest;
  const noDelay = await kept.noDelay;
  return {
    ...common,
    sameScheduler: kept.scheduler === scheduler,
    sameWait: kept.wait === scheduler.wait,
    sameConstructor: kept.Constructor === scheduler.constructor,
    keptWaits: (await kept.wait.call(kept.scheduler, 1)) === undefined,
    topLevelWaitRejected:
      Object.getPrototypeOf(outsideOfARequest) === Error.prototype &&
      outsideOfARequest.message.startsWith('Disallowed operation'),
    topLevelNoDelay: Object.getPrototypeOf(noDelay) === TypeError.prototype,
    topLevelBadReceiver:
      Object.getPrototypeOf(kept.badReceiver) === TypeError.prototype,
  };
}

export default {
  async fetch() {
    return Response.json({
      mode: MODE,
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: await checks(),
    });
  },
};
