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

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { validateObject, validateFunction } from 'node-internal:validators';
import { Buffer } from 'node-internal:internal_buffer';
import { EventEmitter } from 'node-internal:events';

import type { SocketOptions, SocketType, RemoteInfo } from 'node:dgram';

type SocketClassType = typeof Socket;

export function Socket(
  this: SocketClassType,
  type?: SocketType | SocketOptions,
  callback?: (msg: Buffer, rinfo: RemoteInfo) => void
): SocketClassType {
  EventEmitter.call(this as unknown as EventEmitter);
  if (typeof type === 'string') {
    type = { type };
  }
  validateObject(type, 'type');
  if (callback !== undefined) {
    validateFunction(callback, 'callback');
  }
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket');
}
Object.setPrototypeOf(Socket.prototype, EventEmitter.prototype);
Object.setPrototypeOf(Socket, EventEmitter);

export function createSocket(
  type?: SocketType | SocketOptions,
  callback?: (msg: Buffer, rinfo: RemoteInfo) => void
): SocketClassType {
  if (typeof type === 'string') {
    type = { type };
  }
  validateObject(type, 'type');
  if (callback !== undefined) {
    validateFunction(callback, 'callback');
  }
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.createSocket()');
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.bind = function (
  _1: unknown,
  _2: unknown,
  _3: unknown
): SocketClassType {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.bind');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.connect = function (
  _1: unknown,
  _2: unknown,
  _3: unknown
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.connect');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.disconnect = function (): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.disconnect');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.sendto = function (
  _1: unknown,
  _2: unknown,
  _3: unknown,
  _4: unknown,
  _5: unknown,
  _6: unknown
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.sendto');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.send = function (
  _1: unknown,
  _2: unknown,
  _3: unknown,
  _4: unknown,
  _5: unknown,
  _6: unknown
): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.send');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.close = function (_1: unknown): SocketClassType {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dgram.Socket.prototype.close');
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype[Symbol.asyncDispose] = async function (): Promise<void> {
  // no-op
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.address = function (): object {
  return {};
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.remoteAddress = function (): object {
  return {};
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setBroadcast = function (_: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setTTL = function (_: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastTTL = function (_: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastLoopback = function (_: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastInterface = function (_: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.addMembership = function (_1: unknown, _2: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.dropMembership = function (_1: unknown, _2: unknown): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.addSourceSpecificMembership = function (
  _1: unknown,
  _2: unknown,
  _3: unknown
): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.dropSourceSpecificMembership = function (
  _1: unknown,
  _2: unknown,
  _3: unknown
): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.ref = function (this: SocketClassType): SocketClassType {
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.unref = function (this: SocketClassType): SocketClassType {
  return this;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setRecvBufferSize = function (_: number): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setSendBufferSize = function (_: number): void {};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getRecvBufferSize = function (): number {
  return 0;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendBufferSize = function (): number {
  return 0;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendQueueSize = function (): number {
  return 0;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendQueueCount = function (): number {
  return 0;
};

export default {
  createSocket,
  Socket,
};
