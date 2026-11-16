// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';
import {
  immutableApiPrototypes,
  onPrototype,
  setToStringTag,
  specCompliantPropertyAttributes,
  usingTsImpl,
} from 'which-impl';

// A descriptor's shape: 'data:<typeof value>' or 'acc<G><S>', then e, c, w.
function shape(object) {
  const out = {};
  for (const key of Reflect.ownKeys(object)) {
    const d = Object.getOwnPropertyDescriptor(object, key);
    out[String(key)] = [
      'value' in d
        ? `data:${typeof d.value}`
        : `acc${d.get ? 'G' : ''}${d.set ? 'S' : ''}`,
      d.enumerable ? 'e' : '-',
      d.configurable ? 'c' : '-',
      'value' in d ? (d.writable ? 'w' : '-') : '',
    ].join('');
  }
  return out;
}

const kMarks = {
  'Symbol(cloudflare:internal-class)': 'data:symbol---',
  'Symbol(kResourceTypeInspect)': 'data:object-c-',
};

export const globalPlacement = {
  test() {
    const descriptor = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(globalThis),
      'Blob'
    );
    deepStrictEqual(
      [descriptor.writable, descriptor.enumerable, descriptor.configurable],
      [true, true, true]
    );
    strictEqual(descriptor.value, Blob);
    strictEqual(Object.getPrototypeOf(globalThis).File, File);
    for (const name of ['Blob', 'File']) {
      const own = Object.getOwnPropertyDescriptor(globalThis, name);
      if (specCompliantPropertyAttributes) {
        deepStrictEqual(
          [own.writable, own.enumerable, own.configurable],
          [true, false, true]
        );
      } else {
        strictEqual(own, undefined);
      }
    }
  },
};

export const constructorShape = {
  test() {
    deepStrictEqual(shape(Blob), {
      length: 'data:number-c-',
      name: 'data:string-c-',
      // Ledger #3.
      prototype:
        usingTsImpl || immutableApiPrototypes
          ? 'data:object---'
          : 'data:object--w',
    });
    deepStrictEqual(
      [Blob.name, Blob.length, File.name, File.length],
      ['Blob', 0, 'File', 0]
    );
    // Ledger #2.
    strictEqual(Object.getPrototypeOf(File) === Blob, usingTsImpl);
    strictEqual(Object.getPrototypeOf(File.prototype), Blob.prototype);
  },
};

export const prototypeShape = {
  test() {
    const accessors = onPrototype ? { size: 'accGec', type: 'accGec' } : {};
    const tag = setToStringTag
      ? { 'Symbol(Symbol.toStringTag)': 'data:string-cw' }
      : {};
    deepStrictEqual(shape(Blob.prototype), {
      ...accessors,
      slice: 'data:functionecw',
      arrayBuffer: 'data:functionecw',
      bytes: 'data:functionecw',
      text: 'data:functionecw',
      stream: 'data:functionecw',
      constructor: 'data:function-cw',
      ...tag,
      ...kMarks,
    });
    deepStrictEqual(shape(File.prototype), {
      ...(onPrototype ? { name: 'accGec', lastModified: 'accGec' } : {}),
      constructor: 'data:function-cw',
      ...tag,
      ...kMarks,
    });
    for (const name of ['slice', 'arrayBuffer', 'bytes', 'text', 'stream']) {
      strictEqual(Blob.prototype[name].name, name);
      strictEqual(Blob.prototype[name].length, 0);
    }
  },
};

export const instanceShape = {
  test() {
    const file = new File(['a'], 'x', { lastModified: 1 });
    if (onPrototype) {
      deepStrictEqual(shape(file), {});
      strictEqual(JSON.stringify(file), '{}');
    } else {
      // In JSG's order: the subclass's first.
      deepStrictEqual(shape(file), {
        lastModified: 'data:numberec-',
        name: 'data:stringec-',
        type: 'data:stringec-',
        size: 'data:numberec-',
      });
      strictEqual(
        JSON.stringify(file),
        '{"lastModified":1,"name":"x","type":"","size":1}'
      );
      deepStrictEqual(Object.keys(new Blob(['ab'])), ['type', 'size']);
    }
  },
};

export const toStringTag = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new Blob()),
      setToStringTag ? '[object Blob]' : '[object Object]'
    );
    strictEqual(
      Object.prototype.toString.call(new File([], 'f')),
      setToStringTag ? '[object File]' : '[object Object]'
    );
  },
};

export const brandChecks = {
  test() {
    const illegal = { name: 'TypeError', message: /^Illegal invocation/ };
    for (const name of ['slice', 'arrayBuffer', 'bytes', 'text', 'stream']) {
      // Synchronously, not as a rejection.
      let threw = false;
      try {
        Blob.prototype[name].call({});
      } catch (e) {
        threw = true;
        strictEqual(e.name, illegal.name);
        strictEqual(illegal.message.test(e.message), true);
      }
      strictEqual(threw, true, name);
    }
    if (onPrototype) {
      for (const [proto, name, receiver] of [
        [Blob.prototype, 'size', {}],
        [Blob.prototype, 'type', {}],
        [File.prototype, 'name', new Blob()],
        [File.prototype, 'lastModified', new Blob()],
      ]) {
        const { get } = Object.getOwnPropertyDescriptor(proto, name);
        let threw = false;
        try {
          get.call(receiver);
        } catch (e) {
          threw = illegal.message.test(e.message);
        }
        strictEqual(threw, true, name);
      }
    }
  },
};

export const subclassing = {
  async test() {
    class Sub extends Blob {
      constructor() {
        super(['q'], { type: 'A/B' });
      }
    }
    const sub = new Sub();
    strictEqual(sub instanceof Sub, true);
    strictEqual(sub.size, 1);
    strictEqual(sub.type, 'a/b');
    strictEqual(await sub.text(), 'q');
    // Slicing makes a Blob, not a Sub.
    strictEqual(Object.getPrototypeOf(sub.slice()), Blob.prototype);
  },
};
