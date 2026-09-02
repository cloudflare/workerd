// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of CountQueuingStrategy and ByteLengthQueuingStrategy. The
// size FUNCTION's shape carries most of the divergences: the spec (and the
// TypeScript implementation) exposes one shared, non-constructable
// arrow-style function per class; the C++ jsg getter mints a fresh,
// constructable function object on every property access.

import { strictEqual, notStrictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const classes = [CountQueuingStrategy, ByteLengthQueuingStrategy];

export const toStringTagBranding = {
  test() {
    strictEqual(
      Object.prototype.toString.call(
        new CountQueuingStrategy({ highWaterMark: 1 })
      ),
      '[object CountQueuingStrategy]'
    );
    strictEqual(
      Object.prototype.toString.call(
        new ByteLengthQueuingStrategy({ highWaterMark: 1 })
      ),
      '[object ByteLengthQueuingStrategy]'
    );
  },
};

export const accessorPlacementAndBrandChecks = {
  test() {
    for (const Ctor of classes) {
      for (const key of ['highWaterMark', 'size']) {
        const desc = Object.getOwnPropertyDescriptor(Ctor.prototype, key);
        strictEqual(typeof desc.get, 'function', `${Ctor.name}.${key}`);
        throws(() => desc.get.call({}), { name: 'TypeError' });
      }
      strictEqual(
        Object.getOwnPropertyNames(new Ctor({ highWaterMark: 1 })).length,
        0
      );
    }
  },
};

export const highWaterMarkReflectsInit = {
  test() {
    for (const Ctor of classes) {
      const strategy = new Ctor({ highWaterMark: 5.5 });
      strictEqual(strategy.highWaterMark, 5.5);
      strictEqual(strategy.highWaterMark, strategy.highWaterMark);
    }
  },
};

export const sizeFunctionIdentity = {
  test() {
    for (const Ctor of classes) {
      const a = new Ctor({ highWaterMark: 1 });
      const b = new Ctor({ highWaterMark: 2 });
      if (usingTsImpl) {
        strictEqual(a.size, a.size, `${Ctor.name}: stable per instance`);
        strictEqual(a.size, b.size, `${Ctor.name}: shared across instances`);
      } else {
        notStrictEqual(a.size, a.size, `${Ctor.name}: minted per access`);
        notStrictEqual(a.size, b.size);
      }
    }
  },
};

export const sizeFunctionShape = {
  test() {
    for (const Ctor of classes) {
      const size = new Ctor({ highWaterMark: 1 }).size;
      strictEqual(size.name, usingTsImpl ? 'size' : '');
      strictEqual('prototype' in size, !usingTsImpl);
      if (usingTsImpl) {
        throws(() => new size(), { name: 'TypeError' });
      } else {
        ok(new size());
      }
    }
    // Spec .length: BLQS size takes a chunk, CQS size takes none. The C++
    // functions report 0 for both.
    strictEqual(
      new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size.length,
      usingTsImpl ? 1 : 0
    );
    strictEqual(new CountQueuingStrategy({ highWaterMark: 1 }).size.length, 0);
  },
};
