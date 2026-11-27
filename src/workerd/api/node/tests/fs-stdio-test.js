// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Line buffering of writes to the stdio file descriptors.

import { Buffer } from 'node:buffer';
import {
  appendFileSync,
  promises,
  write,
  writeFile,
  writeFileSync,
  writeSync,
  writev,
  writevSync,
} from 'node:fs';
import { deepStrictEqual, strictEqual } from 'node:assert';

// Runs fn with console.log replaced, returning the lines it logged with any
// stdio prefix removed.
async function capture(fn) {
  const lines = [];
  const log = console.log;
  console.log = (msg) => {
    lines.push(String(msg).replace(/^std(out|err): ?/, ''));
  };
  try {
    fn();
    // Lets a pending partial line flush.
    await new Promise((resolve) => setTimeout(resolve, 0));
  } finally {
    console.log = log;
  }
  return { lines };
}

export const splitsLines = {
  async test() {
    for (const fd of [1, 2]) {
      const { lines } = await capture(() => writeSync(fd, 'a\nb\n\nc\n'));
      deepStrictEqual(lines, ['a', 'b', '', 'c']);
    }
  },
};

export const joinsPartialLines = {
  async test() {
    const { lines } = await capture(() => {
      writeSync(1, 'a');
      writeSync(1, 'b\nc');
      writeSync(1, 'd\n');
    });
    deepStrictEqual(lines, ['ab', 'cd']);
  },
};

export const flushesPartialLine = {
  async test() {
    const { lines } = await capture(() => writeSync(1, 'tail'));
    deepStrictEqual(lines, ['tail']);
  },
};

export const limitsWriteSize = {
  async test() {
    let n;
    const { lines } = await capture(() => {
      n = writeSync(1, Buffer.alloc(20000, 'a'));
    });
    strictEqual(n, 16384);
    // Only the last 4 KiB of a partial line are kept.
    deepStrictEqual(lines, ['a'.repeat(4096)]);
  },
};

export const discardsStdin = {
  async test() {
    const { lines } = await capture(() => writeSync(0, 'ignored\n'));
    deepStrictEqual(lines, []);
  },
};

export const writeFileToStdio = {
  async test() {
    const { lines } = await capture(() => {
      writeFileSync(1, 'a\n');
      appendFileSync(2, 'b\n');
    });
    deepStrictEqual(lines, ['a', 'b']);
    const done = Promise.withResolvers();
    const cb = await capture(() => writeFile(1, 'c\n', done.resolve));
    strictEqual(await done.promise, null);
    deepStrictEqual(cb.lines, ['c']);
  },
};

// The file system discards writes to stdio, so every node:fs write API must
// send the stdio fds to internal_fs_stdio.ts.
export const otherWritePaths = {
  async test() {
    const wrote = Promise.withResolvers();
    const wrotev = Promise.withResolvers();
    let written;
    const { lines } = await capture(() => {
      writevSync(1, [Buffer.from('a\n'), Buffer.from('b\n')]);
      write(1, 'c\n', (err, n) => wrote.resolve([err, n]));
      writev(2, [Buffer.from('d\n')], (err, n) => wrotev.resolve([err, n]));
      written = promises.writeFile(1, 'e\n');
    });
    deepStrictEqual(lines, ['a', 'b', 'c', 'd', 'e']);
    deepStrictEqual(await wrote.promise, [null, 2]);
    deepStrictEqual(await wrotev.promise, [null, 2]);
    strictEqual(await written, undefined);
  },
};
