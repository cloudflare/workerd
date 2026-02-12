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

import type {
  ReadStream as ReadStreamType,
  WriteStream as WriteStreamType,
} from 'node:tty';

import type { SocketConstructorOpts } from 'node:net';

// Extended types to include fd property which exists at runtime
interface ReadStreamInstance extends ReadStreamType {
  fd: number;
}

interface WriteStreamInstance extends WriteStreamType {
  fd: number;
}

export function isatty(_fd: number): boolean {
  return false;
}

export function ReadStream(
  this: ReadStreamInstance,
  fd: number,
  _options: SocketConstructorOpts = {}
): ReadStreamInstance {
  this.fd = fd;
  this.isRaw = false;
  this.isTTY = false;
  return this;
}

Object.setPrototypeOf(ReadStream.prototype, Socket.prototype);
Object.setPrototypeOf(ReadStream, Socket);

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ReadStream.prototype.setRawMode = function (
  this: ReadStreamInstance,
  mode: boolean
): ReadStreamInstance {
  this.isRaw = mode;
  return this;
};

export function WriteStream(
  this: WriteStreamInstance,
  fd: number,
  _options: SocketConstructorOpts = {}
): WriteStreamInstance {
  this.fd = fd;
  this.columns = 0;
  this.rows = 0;
  return this;
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
  _y?: number | (() => void),
  _callback?: () => void
): boolean {
  if (typeof _y === 'function') {
    _y();
  } else if (typeof _callback === 'function') {
    _callback();
  }
  return false;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.moveCursor = function (
  _dx: number,
  _dy: number,
  _callback?: () => void
): boolean {
  if (typeof _callback === 'function') {
    _callback();
  }
  return false;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.clearLine = function (
  _dir: number,
  _callback?: () => void
): boolean {
  if (typeof _callback === 'function') {
    _callback();
  }
  return false;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.clearScreenDown = function (
  _callback?: () => void
): boolean {
  if (typeof _callback === 'function') {
    _callback();
  }
  return false;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
WriteStream.prototype.getWindowSize = function (): [number, number] {
  // eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
  return [this.columns ?? 0, this.rows ?? 0];
};

export default { isatty, ReadStream, WriteStream };
