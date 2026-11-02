// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

const sessions = new Map();
let nextSessionId = 1;

class Devtools extends RpcTarget {
  listTargets(sessionId) {
    return [{ id: `${sessionId}-page`, type: 'page', url: 'about:blank' }];
  }
}

export class BrowserBindingWebSocketEntrypoint extends WorkerEntrypoint {
  fetch() {
    const { sessionId, targetId, failUpgrade } = this.ctx.props;
    if (failUpgrade) {
      return new Response('upgrade refused', { status: 503 });
    }

    const [client, server] = new WebSocketPair();
    server.accept();
    server.addEventListener('message', (event) => {
      server.send(JSON.stringify({ sessionId, targetId, echo: event.data }));
    });
    return new Response(null, { status: 101, webSocket: client });
  }
}

export default class extends WorkerEntrypoint {
  async fetch() {
    return Response.json(
      { success: true },
      {
        headers: {
          'content-type': 'application/json',
        },
      }
    );
  }

  quickAction(action, options) {
    return Response.json({ action, url: options.url });
  }

  acquire(options) {
    const sessionId = `session-${nextSessionId++}`;
    // Setting `location` to 'fail-upgrade' makes the pinned Fetcher refuse the upgrade.
    sessions.set(sessionId, options?.location === 'fail-upgrade');
    return { sessionId };
  }

  launch(options) {
    const { sessionId } = this.acquire(options);
    const connection = this.connectSession(sessionId);
    if (options?.targets) {
      connection.targets = new Devtools().listTargets(sessionId);
    }
    return connection;
  }

  connectSession(sessionId, options) {
    if (!sessions.has(sessionId)) {
      throw new Error(`Session not found: ${sessionId}`);
    }
    return {
      sessionId,
      webSocket: this.ctx.exports.BrowserBindingWebSocketEntrypoint({
        props: {
          sessionId,
          targetId: options?.targetId ?? null,
          failUpgrade: sessions.get(sessionId),
        },
      }),
    };
  }

  getLiveView(sessionId, options) {
    return { id: sessionId, options: { mode: options?.mode ?? 'tab' } };
  }

  listSessions() {
    return [...sessions.keys()].map((sessionId) => ({ sessionId }));
  }

  history() {
    return this.listSessions();
  }

  limits() {
    return { maxConcurrentSessions: 10 };
  }

  getSession(sessionId) {
    return sessions.has(sessionId) ? { sessionId } : null;
  }

  closeSession(sessionId) {
    sessions.delete(sessionId);
    return { status: 'closed' };
  }

  get devtools() {
    return new Devtools();
  }
}
