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

async function handleHttp(request) {
  const data = await request.json();
  const reqUrl = new URL(request.url);

  if (request.method !== 'POST') {
    return Response.json({ success: false }, { status: 500 });
  }

  try {
    switch (reqUrl.pathname) {
      case '/get':
        return Response.json({ result: getInstance(data.id) }, { status: 200 });
      case '/create':
        return Response.json({ result: createInstance(data) }, { status: 201 });
      case '/createBatch':
        return Response.json(
          { result: createBatchInstances(data) },
          { status: 201 }
        );
      case '/pause':
      case '/resume':
      case '/terminate':
      case '/send-event':
        return Response.json({ result: null }, { status: 200 });
      case '/restart':
        restartBodies.set(data.id, data);
        return Response.json({ result: null }, { status: 200 });
      case '/status':
        return Response.json(
          { result: instanceStatus(data.id, 'http') },
          { status: 200 }
        );
      case '/last-restart':
        return Response.json(
          { result: restartBodies.get(data.id) ?? null },
          { status: 200 }
        );
      default:
        return Response.json({ success: false }, { status: 404 });
    }
  } catch (err) {
    return Response.json({ error: { message: err.message } }, { status: 500 });
  }
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

  async fetch(request) {
    return handleHttp(request);
  }
}
