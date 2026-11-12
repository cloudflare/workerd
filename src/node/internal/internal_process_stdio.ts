// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// The process.stdin, process.stdout and process.stderr streams. They are built on the
// fs streams, whose module graph is large, so node-internal:public_process loads this
// module on first access to any of them rather than importing it.

import { Buffer } from 'node-internal:internal_buffer';
import { Writable } from 'node-internal:streams_writable';
import { writeSync } from 'node-internal:internal_fs_sync';
import { ReadStream } from 'node-internal:internal_fs_streams';
import type { Readable } from 'node-internal:streams_readable';

// For stdin, we emulate `node foo.js < /dev/null`
// In Node.js, process.stdin is a net.Socket (Duplex) with both readable and writable properties.
// We use 'any' cast to assign socket-like properties without type conflicts.
export const stdin = new ReadStream(null, {
  fd: 0,
  autoClose: false,
}) as Readable & { fd: number };
stdin.fd = 0;
// In Node.js, process.stdin is a net.Socket (Duplex) with socket-like properties.
// We add socket-like stub properties for compatibility, but avoid setting writable-related
// properties that would cause the streams internals to treat this ReadStream as writable
// (which would trigger calls to .end() that don't exist on ReadStream).
/* eslint-disable @typescript-eslint/no-unsafe-assignment,
   @typescript-eslint/no-unsafe-member-access,
   @typescript-eslint/no-explicit-any */
const stdinAny = stdin as any;
stdinAny.allowHalfOpen = false;
stdinAny.bufferSize = 0;
stdinAny.connecting = false;
stdinAny.readyState = 'open';
/* eslint-enable @typescript-eslint/no-unsafe-assignment,
   @typescript-eslint/no-unsafe-member-access,
   @typescript-eslint/no-explicit-any */

function chunkToBuffer(
  chunk: Buffer | ArrayBufferView | DataView | string,
  encoding: BufferEncoding
): Buffer {
  if (Buffer.isBuffer(chunk)) {
    return chunk;
  }
  if (typeof chunk === 'string') {
    return Buffer.from(chunk, encoding);
  }
  return Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
}

// For stdout, we emulate `nohup node foo.js`
class SyncWriteStream extends Writable {
  fd: number;
  override readable: boolean = false;
  _type = 'fs';
  _isStdio = true;
  constructor(fd: number) {
    super({ autoDestroy: true });
    this.fd = fd;
  }
  override _write(
    chunk: string | Buffer | ArrayBufferView | DataView,
    encoding: BufferEncoding,
    cb: (error?: Error | null) => void
  ): void {
    try {
      writeSync(this.fd, chunkToBuffer(chunk, encoding));
    } catch (e: unknown) {
      cb(e as Error);
      return;
    }
    cb();
  }
}

// In Node.js, process.stdout/stderr are net.Socket (TTY) instances with Duplex stream properties.
// We assign socket-like stub properties for compatibility.
/* eslint-disable @typescript-eslint/no-unsafe-assignment,
   @typescript-eslint/no-unsafe-member-access,
   @typescript-eslint/no-explicit-any */
function addSocketProperties(stream: SyncWriteStream): void {
  const s = stream as any;
  s.allowHalfOpen = false;
  s.bufferSize = 0;
  s.bytesRead = 0;
  s.bytesWritten = 0;
  s.connecting = false;
  s.pending = false;
  s.readableAborted = false;
  s.readableDidRead = false;
  s.readableEnded = false;
  s.readableHighWaterMark = 16384;
  s.readableLength = 0;
  s.readableObjectMode = false;
  s.readyState = 'open';
}
/* eslint-enable @typescript-eslint/no-unsafe-assignment,
   @typescript-eslint/no-unsafe-member-access,
   @typescript-eslint/no-explicit-any */

export const stdout = new SyncWriteStream(1);
addSocketProperties(stdout);
export const stderr = new SyncWriteStream(2);
addSocketProperties(stderr);
