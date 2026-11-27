// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A minimal DevTools protocol client over a raw WebSocket. Unlike chrome-remote-interface it
// returns whole parsed messages, responses included, so tests can compare them as JSON values,
// and it records every message from the moment the socket opens, so none sent right after the
// upgrade (such as notifications workerd queued before the session attached) can be missed.

export class CdpClient {
  #ws;
  #nextId = 1;
  #messages = [];
  #pending = new Map();
  #waiters = new Set();
  // Per event method, the index in #messages after the last event returned by waitForEvent().
  #cursors = new Map();
  // Set when the socket closes. A WebSocket drops what is sent once it is closing or closed.
  #closed = false;

  // Resolves with the close event's { code, reason } once the socket closes.
  closed;

  static async connect(port, target = '/main') {
    const ws = new WebSocket(`ws://127.0.0.1:${port}${target}`);
    const client = new CdpClient(ws);
    await new Promise((resolve, reject) => {
      ws.addEventListener('open', resolve, { once: true });
      ws.addEventListener(
        'error',
        () => reject(new Error(`could not connect to ${target}`)),
        { once: true }
      );
    });
    return client;
  }

  constructor(ws) {
    this.#ws = ws;
    ws.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      this.#messages.push(message);
      if (message.id !== undefined) {
        const resolve = this.#pending.get(message.id);
        if (resolve !== undefined) {
          this.#pending.delete(message.id);
          resolve(message);
        }
      }
      for (const waiter of this.#waiters) waiter();
    });
    this.closed = new Promise((resolve) => {
      ws.addEventListener('close', (event) => {
        this.#closed = true;
        for (const resolvePending of this.#pending.values()) {
          resolvePending({ closed: true });
        }
        this.#pending.clear();
        resolve({ code: event.code, reason: event.reason });
      });
    });
  }

  // Every message received so far, in order.
  get messages() {
    return this.#messages;
  }

  // Sends a command and resolves with the whole response message, `id` included, or with
  // `{ closed: true }` if the socket closes first or is no longer open.
  send(method, params) {
    if (this.#closed || this.#ws.readyState !== WebSocket.OPEN) {
      return Promise.resolve({ closed: true });
    }
    const id = this.#nextId++;
    const message =
      params === undefined ? { id, method } : { id, method, params };
    const response = new Promise((resolve) => this.#pending.set(id, resolve));
    this.#ws.send(JSON.stringify(message));
    return response;
  }

  // Sends a command and resolves with its result, or rejects with its error.
  async call(method, params) {
    const response = await this.send(method, params);
    if (response.error !== undefined || response.result === undefined) {
      throw new Error(`${method} failed: ${JSON.stringify(response)}`);
    }
    return response.result;
  }

  // Resolves with the first `method` event matching `predicate` that follows the last one this
  // returned for `method`, whether it arrived before this call or after. The `method` events it
  // skips over are not returned later.
  waitForEvent(method, predicate = () => true) {
    return new Promise((resolve) => {
      const check = () => {
        for (
          let i = this.#cursors.get(method) ?? 0;
          i < this.#messages.length;
          i++
        ) {
          const message = this.#messages[i];
          if (message.method === method && message.id === undefined) {
            this.#cursors.set(method, i + 1);
            if (predicate(message.params)) {
              this.#waiters.delete(check);
              resolve(message);
              return true;
            }
          }
        }
        return false;
      };
      if (!check()) this.#waiters.add(check);
    });
  }

  close() {
    this.#ws.close();
    return this.closed;
  }
}

// Enables the Runtime domain and returns the id of the worker's execution context. workerd marks
// no context as the default one, so Runtime.evaluate needs it.
export async function enableRuntime(client) {
  const created = client.waitForEvent('Runtime.executionContextCreated');
  await client.call('Runtime.enable');
  return (await created).params.context.id;
}
