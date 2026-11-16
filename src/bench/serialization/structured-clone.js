// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone() of object graphs, whose every object the serializer asks its delegate about
// (jsg::Serializer::IsHostObject()), and of arrays and strings, which it does not; then the
// options, whose conversion is the part the C++ and TypeScript implementations do differently.

function tree(depth, width) {
  if (depth === 0) return { leaf: true, n: 1 };
  const node = { depth, children: [] };
  for (let i = 0; i < width; i++) node.children.push(tree(depth - 1, width));
  return node;
}

class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
}

const INPUTS = {
  'empty object': {},
  'flat object 16 keys': Object.fromEntries(
    Array.from({ length: 16 }, (_, i) => [`k${i}`, i])
  ),
  'tree 4x4 (341 objects)': tree(4, 4),
  'tree 6x4 (5461 objects)': tree(6, 4),
  'array of 1000 objects': Array.from({ length: 1000 }, (_, i) => ({ i })),
  'array of 1000 class instances': Array.from(
    { length: 1000 },
    (_, i) => new Point(i, i)
  ),
  'array of 1000 numbers': Array.from({ length: 1000 }, (_, i) => i),
  'string 8192': 'a'.repeat(8192),
};

const SMALL = { a: 1 };
const NO_TRANSFER = {};
const EMPTY_TRANSFER = { transfer: [] };

export default {
  bench(b) {
    for (const [name, input] of Object.entries(INPUTS)) {
      b.run(name, () => b.blackBox(structuredClone(input)));
    }
    b.run('small object, options without transfer', () =>
      b.blackBox(structuredClone(SMALL, NO_TRANSFER))
    );
    b.run('small object, empty transfer', () =>
      b.blackBox(structuredClone(SMALL, EMPTY_TRANSFER))
    );
    // A transferred buffer is detached, so each call needs new ones; the
    // allocations cost the same under both implementations.
    b.run('transfer 1 ArrayBuffer', () => {
      const buffer = new ArrayBuffer(64);
      return b.blackBox(structuredClone(buffer, { transfer: [buffer] }));
    });
    b.run('transfer 16 ArrayBuffers', () => {
      const buffers = Array.from({ length: 16 }, () => new ArrayBuffer(64));
      return b.blackBox(structuredClone(buffers, { transfer: buffers }));
    });
  },
};
