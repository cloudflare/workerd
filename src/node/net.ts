// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
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

import {
  type SocketClass,
  type SocketOptions,
  Socket,
  normalizeArgs,
  isIPv4,
  isIPv6,
  isIP,
} from 'node-internal:internal_net';

import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';

import { validateInt32 } from 'node-internal:validators';

import type { SocketConnectOpts } from 'node:net';

export function BlockList(): void {
  throw new Error('BlockList is not implemented');
}

export function SocketAddress(): void {
  throw new Error('SocketAddress is not implemented');
}

export function Server(): void {
  throw new Error('Server is not implemented');
}

export function connect(...args: unknown[]): SocketClass {
  const normalized = normalizeArgs(args);
  const options = normalized[0] as SocketOptions;
  // @ts-expect-error TS7009 Required for type usage
  const socket: SocketClass = new Socket(options);
  if (options.timeout) {
    socket.setTimeout(options.timeout);
  }
  if (socket.destroyed) {
    return socket;
  }
  return socket.connect(normalized as unknown as SocketConnectOpts);
}

export const createConnection = connect;

export function createServer(): void {
  throw new Error('createServer() is not implemented');
}

export function getDefaultAutoSelectFamily(): boolean {
  // This is the only value we support.
  return false;
}

export function setDefaultAutoSelectFamily(val: unknown): void {
  if (!val) return;
  throw new ERR_INVALID_ARG_VALUE('val', val);
}

// We don't actually make use of this. It's here only for compatibility.
// The value is not used anywhere.
let autoSelectFamilyAttemptTimeout: number = 10;

export function getDefaultAutoSelectFamilyAttemptTimeout(): number {
  return autoSelectFamilyAttemptTimeout;
}

export function setDefaultAutoSelectFamilyAttemptTimeout(val: unknown): void {
  validateInt32(val, 'val', 1);
  if (val < 10) val = 10;
  autoSelectFamilyAttemptTimeout = val as number;
}

export { Socket, isIP, isIPv4, isIPv6 } from 'node-internal:internal_net';

export const _normalizeArgs = normalizeArgs;

export default {
  BlockList,
  SocketAddress,
  Stream: Socket,
  Server,
  Socket,
  connect,
  createConnection,
  createServer,
  getDefaultAutoSelectFamily,
  setDefaultAutoSelectFamily,
  getDefaultAutoSelectFamilyAttemptTimeout,
  setDefaultAutoSelectFamilyAttemptTimeout,
  isIP,
  isIPv4,
  isIPv6,
  _normalizeArgs: normalizeArgs,
};
