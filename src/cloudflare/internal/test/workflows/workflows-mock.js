// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

const restartBodies = new Map();
const subscribeOptions = new Map();

const THROW_ID = 'throw';
const BATCH_ERROR_ID = 'batch-error';
const MISSING_DELETE_ID = 'missing-delete';

class SubscriptionMock extends RpcTarget {
  #events;
  #closed = false;

  constructor(events) {
    super();
    this.#events = events;
  }

  async next() {
    if (this.#closed || this.#events.length === 0) {
      this.#closed = true;
      return { done: true, value: undefined };
    }
    return { done: false, value: this.#events.shift() };
  }
}

export default class WorkflowsMock extends WorkerEntrypoint {
  async getInstance(id) {
    if (id === THROW_ID) {
      throw new Error('workflow instance not found');
    }
    return { id };
  }

  async create(options) {
    return { id: options?.id };
  }

  async createBatch(options) {
    if (Array.isArray(options)) {
      return options.map((val) => ({ id: val.id }));
    }

    const instances =
      options.instances ??
      Array.from({ length: options.count }, (_, index) => ({
        id: `generated-${index}`,
      }));
    const created = [];
    const errors = [];

    for (const [index, instance] of instances.entries()) {
      if (instance.id === BATCH_ERROR_ID) {
        errors.push({
          index,
          id: instance.id,
          code: 10405,
          message: 'Provided instance ID already exists',
        });
      } else {
        created.push({ id: instance.id });
      }
    }

    return { created, errors };
  }

  async deleteBatch(options) {
    return {
      deleted: options.instances
        .filter((id) => id !== MISSING_DELETE_ID)
        .map((id) => ({ id })),
      errors: options.instances
        .filter((id) => id === MISSING_DELETE_ID)
        .map((id) => ({
          id,
          code: 10400,
          message: 'workflows.api.error.instance.not_found',
        })),
    };
  }

  async deleteInstance(_id) {}

  async pause(_id) {}

  async resume(_id) {}

  async terminate(_id) {}

  async restart(id, options) {
    restartBodies.set(id, { ...options, id });
  }

  async status(id) {
    return { status: 'running', output: id };
  }

  async sendEvent(_id, _event) {}

  async subscribe(id, options) {
    subscribeOptions.set(id, options ?? null);

    return new SubscriptionMock([
      {
        instanceId: id,
        eventId: 0,
        timestamp: 0,
        type: 'workflow_completed',
        output: 'done',
      },
    ]);
  }

  // Introspection only. The binding itself never uses fetch(), but the test worker's own compat
  // date leaves RPC gated on `env.mock`, so it reaches these records over HTTP instead.
  async fetch(request) {
    const data = await request.json();
    const pathname = new URL(request.url).pathname;

    switch (pathname) {
      case '/last-restart':
        return Response.json({ result: restartBodies.get(data.id) ?? null });
      case '/last-subscribe':
        return Response.json({ result: subscribeOptions.get(data.id) ?? null });
      default:
        throw new Error(
          `unexpected HTTP request to the workflows mock: ${pathname}`
        );
    }
  }
}
