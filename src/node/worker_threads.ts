// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { EventEmitter } from 'node-internal:events';
import type {
  ResourceLimits,
  Serializable,
  Worker as _Worker,
} from 'node:worker_threads';
import type { Context } from 'node:vm';
import type { Readable, Writable } from 'node:stream';
import type { Transferable, WorkerPerformance } from 'node:worker_threads';
import type { CPUProfileHandle, HeapInfo, HeapProfileHandle } from 'node:v8';

// Import MessageChannel and MessagePort from the internal module to avoid
// dependency on the expose_global_message_channel compatibility flag.
import internalMessageChannel from 'cloudflare-internal:messagechannel';

export const MessageChannel = internalMessageChannel.MessageChannel;
export const MessagePort = internalMessageChannel.MessagePort;

// TODO(soon): Use globalThis.BroadcastChannel once it's available.
export class BroadcastChannel {
  constructor() {
    throw new ERR_METHOD_NOT_IMPLEMENTED('BroadcastChannel');
  }
}

export class Worker extends EventEmitter implements _Worker {
  stdin: Writable | null = null;
  stderr: Readable;
  stdout: Readable;
  threadId: number;
  threadName: string = 'workerd';
  performance: WorkerPerformance;

  constructor() {
    super();
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker');
  }

  cpuUsage(_prev?: NodeJS.CpuUsage): Promise<NodeJS.CpuUsage> {
    return Promise.reject(new ERR_METHOD_NOT_IMPLEMENTED('Worker.cpuUsage'));
  }

  postMessage(_value: unknown, _transferList?: readonly Transferable[]): void {
    // Acts as a no-op
  }

  async postMessageToThread(
    _threadId: unknown,
    _value: unknown,
    _transferList?: unknown,
    _timeout?: unknown
  ): Promise<void> {
    // Acts as a noop.
  }

  ref(): void {
    // Acts as a noop.
  }

  unref(): void {
    // Acts as a noop.
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async terminate(): Promise<number> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker.terminate');
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async getHeapSnapshot(): Promise<Readable> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker.getHeapSnapshot');
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async getHeapStatistics(): Promise<HeapInfo> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker.getHeapStatistics');
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async startCpuProfile(): Promise<CPUProfileHandle> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker.startCpuProfile');
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async startHeapProfile(): Promise<HeapProfileHandle> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Worker.startHeapProfile');
  }

  async [Symbol.asyncDispose](): Promise<void> {
    // Do nothing
  }
}

const environmentData = new Map<string, Serializable>();
export function getEnvironmentData(key: string): Serializable | undefined {
  return environmentData.get(key);
}

export function setEnvironmentData(key: string, value: Serializable): void {
  environmentData.set(key, value);
}

export const isMainThread = true;

export function isMarkedAsUntransferable(_value: unknown): boolean {
  return false;
}

export function markAsUntransferable(_value: unknown): void {
  // This is implement as a no-op.
}

export function markAsUncloneable(_value: unknown): void {
  // This is implement as a no-op.
}

export function moveMessagePortToContext(
  _port: MessagePort,
  _contextifiedSandbox: Context
): MessagePort {
  return new MessagePort();
}

export const parentPort: number | null = null;

export function receiveMessageOnPort(
  _port: MessagePort
): undefined | { message: unknown } {
  return undefined;
}

export const SHARE_ENV = Symbol.for('nodejs.worker_threads.SHARE_ENV');

export const resourceLimits: ResourceLimits = {};

export const threadId: number = 0;

export const workerData: Record<string, unknown> | null = null;

export function postMessageToThread(
  threadId: number,
  value: unknown,
  timeout?: number
): Promise<void>;
export function postMessageToThread(
  _threadId: number,
  _value: unknown,
  _transferList?: number | readonly Transferable[],
  _timeout?: number
): Promise<void> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('postMessageToThread');
}

export const isInternalThread = false;

export default {
  BroadcastChannel,
  MessageChannel,
  MessagePort,
  Worker,
  SHARE_ENV,
  getEnvironmentData,
  isMainThread,
  isMarkedAsUntransferable,
  markAsUntransferable,
  markAsUncloneable,
  moveMessagePortToContext,
  parentPort,
  receiveMessageOnPort,
  resourceLimits,
  setEnvironmentData,
  postMessageToThread,
  threadId,
  workerData,
  isInternalThread,
};
