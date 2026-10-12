// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

// Keeps rejected promises reachable so that GC cannot release their pipelines
// and hide a pipeline that the runtime failed to drop.
const held = [];

export class Callee extends WorkerEntrypoint {
  async throwsDirectly() {
    throw new Error('boom from callee');
  }

  async throwsWithMessage(message) {
    throw new Error(message);
  }

  async rejectsOnCallerStub(step) {
    const promise = step.fail();
    held.push(promise);
    await assert.rejects(promise, { message: 'step failed' });
    return 'ok';
  }

  async retriesCallback(step) {
    let attempts = 0;
    return await step.retry(async () => {
      if (attempts++ === 0) throw new Error('first attempt fails');
      return 'ok';
    });
  }
}

export class CalleeActor extends DurableObject {
  async actorThrowsDirectly() {
    throw new Error('boom from actor');
  }
}

export default {};
