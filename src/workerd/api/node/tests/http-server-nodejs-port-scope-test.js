// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';
import http from 'node:http';
import { handleAsNodeRequest } from 'cloudflare:node';
import { DurableObject } from 'cloudflare:workers';

const SHARED_PORT = 18080;
const DURABLE_PORT = 18081;
const EPHEMERAL_PORT = 18082;

async function requestPort(port, body) {
  const response = await handleAsNodeRequest(
    { port },
    new Request('https://example.com/', { method: 'POST', body })
  );
  return response.text();
}

export class PortHost extends DurableObject {
  #label;
  #server;

  async listen(port, label) {
    this.#label = label;
    this.#server = http.createServer((request, response) => {
      let body = '';
      request.setEncoding('utf8');
      request.on('data', (chunk) => (body += chunk));
      request.on('end', () => {
        response.write(this.#label);
        queueMicrotask(() => response.end(`:${body}`));
      });
    });
    await new Promise((resolve) => this.#server.listen(port, resolve));
  }

  request(port, body) {
    return requestPort(port, body);
  }

  close() {
    this.#server.close();
  }
}

export class SharedPortHost extends PortHost {}
export class DurablePortHost extends PortHost {}
export class EphemeralPortHost extends PortHost {}

// Local-development runners evaluate user modules inside a pinned artificial
// actor, then invoke their exports from the stateless worker. The marked actor
// therefore registers its Node server in the isolate table.
export const testPinnedRunnerActorUsesIsolatePortScope = {
  async test(_controller, env) {
    const host = env.SHARED.get(env.SHARED.idFromName('singleton'));
    await host.listen(SHARED_PORT, 'shared');

    strictEqual(await requestPort(SHARED_PORT, 'request'), 'shared:request');
    await host.close();
  },
};

export const testDurableObjectPortScopesRemainIsolated = {
  async test(_controller, env) {
    const a = env.DURABLE.get(env.DURABLE.idFromName('a'));
    const b = env.DURABLE.get(env.DURABLE.idFromName('b'));
    await a.listen(DURABLE_PORT, 'a');
    await b.listen(DURABLE_PORT, 'b');

    strictEqual(await a.request(DURABLE_PORT, 'request'), 'a:request');
    strictEqual(await b.request(DURABLE_PORT, 'request'), 'b:request');
    await rejects(requestPort(DURABLE_PORT, 'request'), {
      message: /^Http server with port 18081 not found/,
    });

    await a.close();
    await b.close();
  },
};

export const testEphemeralObjectPortScopesRemainIsolated = {
  async test(_controller, env) {
    const a = env.EPHEMERAL.get('a');
    const b = env.EPHEMERAL.get('b');
    await a.listen(EPHEMERAL_PORT, 'a');
    await b.listen(EPHEMERAL_PORT, 'b');

    strictEqual(await a.request(EPHEMERAL_PORT, 'request'), 'a:request');
    strictEqual(await b.request(EPHEMERAL_PORT, 'request'), 'b:request');
    await rejects(requestPort(EPHEMERAL_PORT, 'request'), {
      message: /^Http server with port 18082 not found/,
    });

    await a.close();
    await b.close();
  },
};
