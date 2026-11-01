// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Line buffering of writes to the stdio file descriptors. The same module
// runs under the C++ implementation (fs-stdio-cpp.wd-test) and the TypeScript
// one (fs-stdio-ts.wd-test). The TS_STDIO autogate, which user code cannot
// read, selects between them, so each cell says which one it is meant to run
// through its USING_TS_IMPL binding.

import { env } from 'cloudflare:workers';
import { Buffer } from 'node:buffer';
import { appendFileSync, writeFile, writeFileSync, writeSync } from 'node:fs';
import { deepStrictEqual, strictEqual } from 'node:assert';

const usingTsImpl = env.USING_TS_IMPL === true;

// Runs fn with console.log replaced, returning the lines it logged with any
// stdio prefix removed, and the stack of the first call.
async function capture(fn) {
  const lines = [];
  let stack;
  const log = console.log;
  console.log = (msg) => {
    stack ??= new Error().stack;
    lines.push(String(msg).replace(/^std(out|err): ?/, ''));
  };
  try {
    fn();
    // Lets a pending partial line flush.
    await new Promise((resolve) => setTimeout(resolve, 0));
  } finally {
    console.log = log;
  }
  return { lines, stack };
}

export const whichImpl = {
  async test() {
    const { stack } = await capture(() => writeSync(1, 'x\n'));
    strictEqual(stack.includes('internal_fs_stdio'), usingTsImpl);
  },
};

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
