// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// Usage of `any` is required because types/node is using it...
/* eslint-disable @typescript-eslint/no-explicit-any */

import type {
  Cluster as _Cluster,
  Worker as _Worker,
  ClusterSettings,
} from 'node:cluster';
import { EventEmitter } from 'node-internal:events';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export const SCHED_NONE = 1;
export const SCHED_RR = 2;

export const isMaster = true;
export const isPrimary = true;
export const isWorker = false;

export const schedulingPolicy = SCHED_RR;
export const settings: ClusterSettings = {};
export const workers: NodeJS.Dict<Worker> = {};

export function fork(_env?: unknown): Worker {
  throw new ERR_METHOD_NOT_IMPLEMENTED('cluster.fork');
}

export function disconnect(_callback?: () => void): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('cluster.disconnect');
}

export function setupPrimary(_settings?: ClusterSettings): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('cluster.setupPrimary');
}

export function setupMaster(_settings?: ClusterSettings): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('cluster.setupWorker');
}

export const _events: unknown[] = [];
export const _eventsCount = 0;
export const _maxListeners = 0;

export class Worker extends EventEmitter implements _Worker {
  _connected: boolean = false;
  id = 0;
  get process(): any {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-return
    return globalThis.process as any;
  }
  get exitedAfterDisconnect(): boolean {
    return this._connected;
  }
  isConnected(): boolean {
    return this._connected;
  }
  isDead(): boolean {
    return true;
  }
  send(
    _message: any,
    _sendHandle?: any,
    _options?: any,
    _callback?: any
  ): boolean {
    return false;
  }
  kill(_signal?: string): void {
    this._connected = false;
  }
  destroy(_signal?: string): void {
    this._connected = false;
  }
  disconnect(): this {
    this._connected = false;
    return this;
  }
}

export class Cluster extends EventEmitter implements _Cluster {
  Worker = Worker;
  isMaster = isMaster;
  isPrimary = isPrimary;
  isWorker = isWorker;
  SCHED_NONE = SCHED_NONE;
  SCHED_RR = SCHED_RR;
  schedulingPolicy = SCHED_RR;
  settings = settings;
  workers = workers;
  setupPrimary(settings?: ClusterSettings): void {
    setupPrimary(settings);
  }
  setupMaster(settings?: ClusterSettings): void {
    setupMaster(settings);
  }
  disconnect(): void {
    disconnect();
  }
  fork(env?: any): Worker {
    return fork(env);
  }
}

export default new Cluster();
