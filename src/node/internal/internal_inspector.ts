// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// Shared implementation of the real (local-dev-only) `node:inspector` Session, backed by the
// isolate's own V8 inspector via the `node-internal:inspector` native Connection. Modeled on
// Node.js's lib/inspector.js: the CDP envelope (id/method/params/result/error) is managed here in
// JavaScript; the native Connection is a thin string pipe. Both the callback-based `Session`
// (node:inspector) and the promise-based `Session` (node:inspector/promises) are derived from a
// common base so the two public modules stay thin.

import { EventEmitter } from 'node-internal:events';
import {
  ERR_INSPECTOR_ALREADY_CONNECTED,
  ERR_INSPECTOR_CLOSED,
  ERR_INSPECTOR_COMMAND,
  ERR_INSPECTOR_NOT_CONNECTED,
  ERR_INSPECTOR_NOT_WORKER,
  ERR_INVALID_ARG_TYPE,
} from 'node-internal:internal_errors';
import { default as inspectorImpl } from 'node-internal:inspector';
import type { Connection as InternalConnection } from 'node-internal:inspector';

export type PostCallback = (err: Error | null, params?: object) => void;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

// Common machinery shared by the callback- and promise-based Sessions.
abstract class BaseSession extends EventEmitter {
  #connection: InternalConnection | null = null;
  #nextId: number = 1;
  #messageCallbacks: Map<number, PostCallback> = new Map();

  connect(): void {
    if (this.#connection) {
      throw new ERR_INSPECTOR_ALREADY_CONNECTED('The inspector session');
    }
    this.#connection = new inspectorImpl.Connection((message: string): void => {
      this.#onMessage(message);
    });
  }

  connectToMainThread(): void {
    // Worker threads are not supported in workerd, so there is no main thread to connect to.
    // Match Node.js, which throws ERR_INSPECTOR_NOT_WORKER when not running on a worker thread.
    throw new ERR_INSPECTOR_NOT_WORKER();
  }

  disconnect(): void {
    if (!this.#connection) {
      return;
    }
    this.#connection.disconnect();
    this.#connection = null;
    const remainingCallbacks = [...this.#messageCallbacks.values()];
    for (const callback of remainingCallbacks) {
      queueMicrotask(() => {
        callback(new ERR_INSPECTOR_CLOSED());
      });
    }
    this.#messageCallbacks.clear();
    this.#nextId = 1;
  }

  // Core dispatch used by both the callback and promise `post` implementations.
  protected postMessage(
    method: unknown,
    params: unknown,
    callback?: PostCallback
  ): void {
    if (typeof method !== 'string') {
      throw new ERR_INVALID_ARG_TYPE('method', 'string', method);
    }
    if (!this.#connection) {
      throw new ERR_INSPECTOR_NOT_CONNECTED();
    }
    const id = this.#nextId++;
    const message: { id: number; method: string; params?: object } = {
      id,
      method,
    };
    if (isObject(params)) {
      message.params = params;
    }
    if (callback) {
      this.#messageCallbacks.set(id, callback);
    }
    this.#connection.dispatch(JSON.stringify(message));
  }

  #onMessage(message: string): void {
    // This runs inside the native dispatch() call, so we must never throw synchronously back through
    // the inspector. JSON.parse is therefore inside the try/catch: CDP data crosses a string
    // boundary, and a malformed payload (SyntaxError) must be surfaced asynchronously rather than
    // unwinding here. We validate the parsed shape rather than blindly casting; the V8 inspector
    // always emits JSON objects, so anything that isn't an object is ignored.
    try {
      const parsed: unknown = JSON.parse(message);
      if (!isObject(parsed)) {
        return;
      }
      const id = parsed['id'];
      if (typeof id === 'number') {
        const callback = this.#messageCallbacks.get(id);
        this.#messageCallbacks.delete(id);
        if (callback) {
          const error = parsed['error'];
          if (isObject(error)) {
            const code = typeof error['code'] === 'number' ? error['code'] : -1;
            const errorMessage =
              typeof error['message'] === 'string' ? error['message'] : '';
            callback(new ERR_INSPECTOR_COMMAND(code, errorMessage));
            return;
          }
          const result = parsed['result'];
          callback(null, isObject(result) ? result : undefined);
        }
      } else if (typeof parsed['method'] === 'string') {
        // A CDP notification (no id). Emit under both the method name and a catch-all event,
        // matching Node's behavior. The full message (including `params`) is forwarded.
        this.emit(parsed['method'], parsed);
        this.emit('inspectorNotification', parsed);
      }
    } catch (error) {
      // Never throw synchronously here: this runs inside the native dispatch() call, so we must not
      // unwind back through the inspector. reportError() surfaces it to the global 'error' event
      // instead of silently swallowing it.
      reportError(error);
    }
  }
}

// Callback-based Session (node:inspector).
export class Session extends BaseSession {
  post(method: unknown, params?: unknown, callback?: unknown): void {
    // `post(method, callback)` shorthand: the callback may arrive as the second argument.
    if (!callback && typeof params === 'function') {
      callback = params;
      params = undefined;
    }
    // `callback` is typed `unknown`; only treat it as a callback once we've confirmed it's callable.
    const cb: PostCallback | undefined =
      typeof callback === 'function' ? (callback as PostCallback) : undefined;
    this.postMessage(method, params, cb);
  }
}

// Promise-based Session (node:inspector/promises).
export class PromiseSession extends BaseSession {
  post(method: unknown, params?: unknown): Promise<object | undefined> {
    const { promise, resolve, reject } = Promise.withResolvers<
      object | undefined
    >();
    this.postMessage(method, params, (err: Error | null, result?: object) => {
      if (err) {
        reject(err);
      } else {
        resolve(result);
      }
    });
    return promise;
  }
}
