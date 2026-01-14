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

import type {
  Socket as DgramSocket,
  RemoteInfo,
  SocketOptions,
  SocketType,
} from 'node:dgram'
import { EventEmitter } from 'node-internal:events'
import type { Buffer } from 'node-internal:internal_buffer'
import { validateFunction, validateObject } from 'node-internal:validators'

type SocketClassType = typeof DgramSocket

export function Socket(
  this: SocketClassType,
  type?: SocketType | SocketOptions,
  callback?: (msg: Buffer, rinfo: RemoteInfo) => void,
): SocketClassType {
  EventEmitter.call(this as unknown as EventEmitter)
  if (typeof type === 'string') {
    type = { type }
  }
  validateObject(type, 'type')
  if (callback !== undefined) {
    validateFunction(callback, 'callback')
  }
  return this
}
Object.setPrototypeOf(Socket.prototype, EventEmitter.prototype)
Object.setPrototypeOf(Socket, EventEmitter)

export function createSocket(
  type?: SocketType | SocketOptions,
  callback?: (msg: Buffer, rinfo: RemoteInfo) => void,
): SocketClassType {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any, @typescript-eslint/no-unsafe-call
  return new (Socket as any)(type, callback) as SocketClassType
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.bind = function (
  this: SocketClassType,
  _1: unknown,
  _2: unknown,
  _3: unknown,
): SocketClassType {
  return this
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.connect = (_1: unknown, _2: unknown, _3: unknown): void => {
  // no-op
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.disconnect = (): void => {
  // no-op
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.sendto = (
  _1: unknown,
  _2: unknown,
  _3: unknown,
  _4: unknown,
  _5: unknown,
  _6: unknown,
): void => {
  // no-op
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.send = (
  _1: unknown,
  _2: unknown,
  _3: unknown,
  _4: unknown,
  _5: unknown,
  _6: unknown,
): void => {
  // no-op
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.close = function (
  this: SocketClassType,
  _1: unknown,
): SocketClassType {
  return this
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype[Symbol.asyncDispose] = async (): Promise<void> => {
  // no-op
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.address = (): object => ({})

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.remoteAddress = (): object => ({})

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setBroadcast = (_: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setTTL = (_: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastTTL = (_: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastLoopback = (_: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setMulticastInterface = (_: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.addMembership = (_1: unknown, _2: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.dropMembership = (_1: unknown, _2: unknown): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.addSourceSpecificMembership = (
  _1: unknown,
  _2: unknown,
  _3: unknown,
): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.dropSourceSpecificMembership = (
  _1: unknown,
  _2: unknown,
  _3: unknown,
): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.ref = function (this: SocketClassType): SocketClassType {
  return this
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.unref = function (this: SocketClassType): SocketClassType {
  return this
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setRecvBufferSize = (_: number): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.setSendBufferSize = (_: number): void => {}

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getRecvBufferSize = (): number => 0

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendBufferSize = (): number => 0

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendQueueSize = (): number => 0

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
Socket.prototype.getSendQueueCount = (): number => 0

export default {
  createSocket,
  Socket,
}
