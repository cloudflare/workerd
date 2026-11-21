// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:buffer transcode() for each conversion path, on ASCII and non-ASCII input.

import { Buffer, transcode } from 'node:buffer';

const LENGTHS = [64, 1024, 65536];

// [from, to, content], where content is the code point every character of the
// input repeats.
const PAIRS = [
  ['latin1', 'utf8', 'a'],
  ['latin1', 'utf8', '\xe9'],
  ['ascii', 'utf8', 'a'],
  ['ascii', 'ascii', 'a'],
  ['ascii', 'latin1', 'a'],
  ['latin1', 'ascii', 'a'],
  ['latin1', 'latin1', 'a'],
  ['utf8', 'latin1', 'a'],
  ['utf8', 'ascii', 'a'],
  ['utf8', 'latin1', '\xe9'],
  ['utf8', 'utf8', 'a'],
  ['utf8', 'utf8', '€'],
  ['ucs2', 'ucs2', 'a'],
  ['ucs2', 'latin1', 'a'],
];

export default {
  bench(b) {
    for (const [from, to, char] of PAIRS) {
      const content = char === 'a' ? 'ascii' : 'non-ascii';
      for (const len of LENGTHS) {
        const source = Buffer.from(char.repeat(len), from);
        b.run(`${from}->${to} ${content} ${len}`, () =>
          b.blackBox(transcode(source, from, to))
        );
      }
    }
  },
};
