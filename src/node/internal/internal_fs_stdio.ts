// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writes to the stdio file descriptors (0, 1, 2) are line-buffered and logged
// through globalThis.console.log, which gives them inspector reporting and
// structured logging. Writes to stdin are discarded. Synchronous writes join
// into one line; a partial line is flushed in a microtask.

import { default as cffs } from 'cloudflare-internal:filesystem';

const MAX_WRITE_SIZE = 16 * 1024;
const MAX_LINE_BUFFER_SIZE = 4096;
const NEWLINE = 0x0a;

const decoder = new TextDecoder('utf-8', { ignoreBOM: true });

class StdioStream {
  readonly #fd: number;
  #pending: Uint8Array = new Uint8Array(0);
  #scheduled = false;

  constructor(fd: number) {
    this.#fd = fd;
  }

  write(bytes: Uint8Array): void {
    let pos = 0;
    while (pos < bytes.length) {
      const nl = bytes.indexOf(NEWLINE, pos);
      if (nl === -1) {
        this.#buffer(bytes.subarray(pos));
        return;
      }
      const line = bytes.subarray(pos, nl);
      pos = nl + 1;
      if (this.#pending.length > 0) {
        const head = this.#pending;
        this.#pending = new Uint8Array(0);
        this.#log(concat(head, line));
      } else {
        this.#log(line);
      }
    }
  }

  // Keeps the last MAX_LINE_BUFFER_SIZE bytes of the partial line.
  #buffer(rest: Uint8Array): void {
    const all = concat(this.#pending, rest);
    this.#pending = all.subarray(
      Math.max(0, all.length - MAX_LINE_BUFFER_SIZE)
    );
    if (this.#scheduled) return;
    this.#scheduled = true;
    queueMicrotask(() => {
      this.#scheduled = false;
      if (this.#pending.length === 0) return;
      const line = this.#pending;
      this.#pending = new Uint8Array(0);
      this.#log(line);
    });
  }

  #log(line: Uint8Array): void {
    const console = globalThis.console as unknown;
    if (typeof console !== 'object' || console === null) return;
    const log = (console as { log?: unknown }).log;
    if (typeof log !== 'function') return;
    const prefix = cffs.stdioPrefix(this.#fd);
    const text = decoder.decode(line);
    let msg = text;
    if (text.length === 0) msg = prefix;
    else if (prefix.length > 0) msg = `${prefix} ${text}`;
    log.call(console, msg);
  }
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length);
  out.set(a);
  out.set(b, a.length);
  return out;
}

const streams = [null, new StdioStream(1), new StdioStream(2)];

const enabled = cffs.tsStdio();

// When false, writes to the stdio file descriptors go to the native file
// system, which line-buffers and logs them itself.
export function isStdioFd(fd: number | URL): fd is number {
  return enabled && (fd === 0 || fd === 1 || fd === 2);
}

// Returns the number of bytes accepted: each buffer is truncated to
// MAX_WRITE_SIZE.
export function writeStdio(
  fd: number,
  buffers: readonly NodeJS.ArrayBufferView[]
): number {
  const chunks = buffers.map((b) => {
    const n = Math.min(b.byteLength, MAX_WRITE_SIZE);
    return n === 0
      ? new Uint8Array(0)
      : new Uint8Array(b.buffer, b.byteOffset, n).slice();
  });
  let total = 0;
  for (const chunk of chunks) {
    streams[fd]?.write(chunk);
    total += chunk.length;
  }
  return total;
}
