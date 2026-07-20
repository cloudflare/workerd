// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

const restartBodies = new Map();

const THROW_ID = 'throw';

function getInstance(id) {
  if (id === THROW_ID) {
    throw new Error('workflow instance not found');
  }
  return { id };
}

function createInstance(options) {
  return { id: options?.id };
}

function createBatchInstances(options) {
  return options.map((val) => ({ id: val.id }));
}

function instanceStatus(id, transport) {
  return { status: 'running', output: id, transport };
}

export default class WorkflowsMock extends WorkerEntrypoint {
  async getInstance(id) {
    return getInstance(id);
  }

  async create(options) {
    return createInstance(options);
  }

  async createBatch(options) {
    return createBatchInstances(options);
  }

  async pause(_id) {}

  async resume(_id) {}

  async terminate(_id) {}

  async restart(id, options) {
    restartBodies.set(id, { ...options, id });
  }

  async status(id) {
    return instanceStatus(id, 'rpc');
  }

  async sendEvent(_id, _event) {}

  async lastRestart(id) {
    return restartBodies.get(id) ?? null;
  }
}
