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

import { Socket } from 'node-internal:internal_net';

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

import type {
  ReadStream as ReadStreamType,
  WriteStream as WriteStreamType,
} from 'node:tty';

import type { SocketConstructorOpts } from 'node:net';

export function isatty(_fd: number): boolean {
  return false;
}

export function ReadStream(
  this: ReadStreamType,
  _fd: number,
  _options: SocketConstructorOpts = {}
): ReadStreamType {
  throw new ERR_METHOD_NOT_IMPLEMENTED('ReadStream');
}

Object.setPrototypeOf(ReadStream.prototype, Socket.prototype);
Object.setPrototypeOf(ReadStream, Socket);

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ReadStream.prototype.setRawMode = function (_flag: boolean): ReadStreamType {
  // no-op.. really no reason to throw here.
  return this as ReadStreamType;
};

export function WriteStream(
  this: WriteStreamType,
  _fd: number,
  _options: SocketConstructorOpts = {}
): WriteStreamType {
  throw new ERR_METHOD_NOT_IMPLEMENTED('WriteStream');
}

Object.setPrototypeOf(WriteStream.prototype, Socket.prototype);
Object.setPrototypeOf(WriteStream, Socket);

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.isTTY = true;

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.getColorDepth = function (): number {
  return 8; // In Node.js, this means 256 colors, but we don't support colors at all.
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.hasColors = function (): boolean {
  return false;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype._refreshSize = function (): void {
  // no-op
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.cursorTo = function (
  _x: number,
  _y: number,
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  _callback: Function
): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('cursorTo');
};
// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.moveCursor = function (
  _dx: number,
  _dy: number,
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  _callback: Function
): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('moveCursor');
};
// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.clearLine = function (
  _dir: number,
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  _callback: Function
): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('clearLine');
};
// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.clearScreenDown = function (
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  _callback: Function
): boolean {
  throw new ERR_METHOD_NOT_IMPLEMENTED('clearScreenDown');
};
// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.getWindowSize = function (): [number, number] {
  return [0, 0];
};

export default { isatty, ReadStream, WriteStream };
