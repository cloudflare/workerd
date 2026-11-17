// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { rejectResizable } from 'which-impl';

const notAnArray = {
  name: 'TypeError',
  message:
    "Failed to construct 'Blob': constructor parameter 1 is not of type 'Array'.",
};

async function bytesOf(blob) {
  return [...(await blob.bytes())];
}

export const constructorRequiresNew = {
  test() {
    for (const [name, construct] of [
      ['Blob', () => Blob([])],
      ['File', () => File([], 'f')],
    ]) {
      throws(construct, {
        name: 'TypeError',
        message: `Failed to construct '${name}': Please use the 'new' operator, this object constructor cannot be called as a function.`,
      });
    }
  },
};

export const parts = {
  async test() {
    strictEqual(new Blob().size, 0);
    strictEqual(new Blob(undefined).size, 0);
    for (const value of [
      null,
      1,
      'abc',
      new Set(['a']),
      { length: 1, 0: 'a' },
      // JSG's IsArray() rejects a proxy, even over an array.
      new Proxy(['a'], {}),
    ]) {
      throws(() => new Blob(value), notAnArray);
    }
    // Anything but a Blob or a BufferSource is converted to a string; a hole
    // reads as undefined.
    const mixed = new Blob([1, null, undefined, {}, true, 'é']);
    strictEqual(await mixed.text(), '1nullundefined[object Object]trueé');
    // eslint-disable-next-line no-sparse-arrays
    strictEqual(new Blob([, 'a']).size, 10);
    throws(() => new Blob([Symbol('x')]), {
      name: 'TypeError',
      message: 'Cannot convert a Symbol value to a string',
    });
    // A SharedArrayBuffer is not a BufferSource, so it becomes its string.
    strictEqual(
      await new Blob([new SharedArrayBuffer(2)]).text(),
      '[object SharedArrayBuffer]'
    );
    // A view over a SharedArrayBuffer is one.
    strictEqual(new Blob([new Uint8Array(new SharedArrayBuffer(3))]).size, 3);
    deepStrictEqual(
      await bytesOf(new Blob([new DataView(new ArrayBuffer(3))])),
      [0, 0, 0]
    );
    strictEqual(new Blob([new Float64Array(1)]).size, 8);
    deepStrictEqual(
      await bytesOf(new Blob([new Uint8Array([1, 2, 3, 4]).subarray(1, 3)])),
      [2, 3]
    );
    strictEqual(await new Blob([new Blob(['ab']), 'c']).text(), 'abc');
    strictEqual(await new Blob([new File(['xy'], 'a')]).text(), 'xy');
  },
};

export const sourcesAreCopied = {
  async test() {
    const source = new Uint8Array([97]);
    const blob = new Blob([source]);
    source[0] = 98;
    strictEqual(await blob.text(), 'a');
  },
};

export const detachedSources = {
  test() {
    const buffer = new ArrayBuffer(4);
    const view = new DataView(buffer);
    const array = new Uint8Array(buffer);
    structuredClone(buffer, { transfer: [buffer] });
    strictEqual(new Blob([buffer, view, array]).size, 0);
  },
};

export const conversionOrder = {
  test() {
    const log = [];
    const part = {
      toString() {
        log.push('part');
        return 'p';
      },
    };
    new Blob([part], {
      get type() {
        log.push('type');
        return 't';
      },
      get endings() {
        log.push('endings');
        return undefined;
      },
    });
    deepStrictEqual(log, ['part', 'type', 'endings']);
    log.length = 0;
    new File(
      [part],
      {
        toString() {
          log.push('name');
          return 'n';
        },
      },
      {
        get type() {
          log.push('type');
          return 't';
        },
        get lastModified() {
          log.push('lastModified');
          return 1;
        },
        get endings() {
          log.push('endings');
          return undefined;
        },
      }
    );
    deepStrictEqual(log, ['part', 'name', 'type', 'lastModified', 'endings']);
  },
};

export const options = {
  test() {
    strictEqual(new Blob([], null).type, '');
    strictEqual(
      new Blob(
        [],
        Object.assign(() => {}, { type: 'f/n' })
      ).type,
      'f/n'
    );
    for (const value of [1, 'x', true]) {
      throws(() => new Blob([], value), {
        name: 'TypeError',
        message:
          "Failed to construct 'Blob': constructor parameter 2 is not of type 'Options'.",
      });
    }
    throws(() => new Blob([], { endings: 'native' }), {
      name: 'Error',
      message: "The 'endings' field on 'Options' is not implemented.",
    });
    strictEqual(new Blob([], { endings: undefined }).size, 0);
  },
};

export const typeNormalization = {
  test() {
    const type = (t) => new Blob([], { type: t }).type;
    strictEqual(type('TEXT/Plain;A=B'), 'text/plain;a=b');
    strictEqual(type('téxt'), '');
    strictEqual(type('a\u001fb'), '');
    strictEqual(type('a\ud800'), '');
    // DEL is kept.
    strictEqual(type('a\u007fb'), 'a\u007fb');
    strictEqual(
      type({
        toString() {
          return 'X/Y';
        },
      }),
      'x/y'
    );
    strictEqual(type(null), 'null');
    strictEqual(type(undefined), '');
  },
};

// Strings become bytes as JSG makes a kj::String of them: as WTF-8, a lone
// surrogate as its own three bytes, a pair as the code point it encodes.
export const loneSurrogates = {
  async test() {
    deepStrictEqual(
      await bytesOf(new Blob(['a\ud800b'])),
      [97, 0xed, 0xa0, 0x80, 98]
    );
    deepStrictEqual(
      await bytesOf(new Blob(['\udc00😀\ud800'])),
      [0xed, 0xb0, 0x80, 0xf0, 0x9f, 0x98, 0x80, 0xed, 0xa0, 0x80]
    );
    // A File's name is kept the same way, and read as those bytes decoded.
    const file = new File([], 'a\ud800b');
    strictEqual(file.name, 'a���b');
    const form = new FormData();
    form.append('f', file);
    // Each byte as the code unit of the same value.
    const body = String.fromCharCode(
      ...new Uint8Array(await new Response(form).arrayBuffer())
    );
    strictEqual(body.includes('filename="a\xed\xa0\x80b"'), true);
  },
};

// File's constructor calls nothing a script can replace: with File's
// [[Prototype]] swapped for a function returning an existing Blob, a new File
// is still a new File, and the Blob is untouched.
export const fileConstructorIgnoresItsPrototype = {
  async test() {
    const victim = new Blob(['safe'], { type: 'a/b' });
    let called = false;
    const original = Object.getPrototypeOf(File);
    Object.setPrototypeOf(File, function () {
      called = true;
      return victim;
    });
    let file;
    try {
      file = new File(['new'], 'n');
    } finally {
      Object.setPrototypeOf(File, original);
    }
    strictEqual(called, false);
    strictEqual(file === victim, false);
    strictEqual(await file.text(), 'new');
    strictEqual(file.name, 'n');
    strictEqual(await victim.text(), 'safe');
    strictEqual(victim.type, 'a/b');
    strictEqual(await structuredClone(victim).text(), 'safe');
  },
};

export const files = {
  async test() {
    throws(() => new File(), {
      name: 'TypeError',
      message:
        "Failed to construct 'File': constructor parameter 2 is not of type 'string'.",
    });
    throws(() => new File([]), TypeError);
    strictEqual(new File([], null).name, 'null');
    strictEqual(
      new File([], {
        toString() {
          return 'n';
        },
      }).name,
      'n'
    );
    const before = Date.now();
    const file = new File(['xy'], 'f', { type: 'A/B' });
    strictEqual(file.type, 'a/b');
    strictEqual(file.size, 2);
    strictEqual(
      file.lastModified >= before && file.lastModified <= Date.now(),
      true
    );
    const lastModified = (value) =>
      new File([], 'n', { lastModified: value }).lastModified;
    strictEqual(lastModified(1.5), 1.5);
    strictEqual(lastModified(NaN), 0);
    strictEqual(lastModified(Infinity), Infinity);
    strictEqual(lastModified('12'), 12);
    strictEqual(lastModified(2 ** 64), 2 ** 64);
    strictEqual(lastModified(null), 0);
    strictEqual(lastModified({}), 0);
    throws(() => new File([], 'n', 1), {
      name: 'TypeError',
      message:
        "Failed to construct 'File': constructor parameter 3 is not of type 'Options'.",
    });
    strictEqual(await new File([new File(['xy'], 'a')], 'b').text(), 'xy');
  },
};

export const sizeLimit = {
  test() {
    // workerd's limit is 128 MiB; the parts' sizes are checked before
    // anything is copied.
    const part = new Uint8Array(16 * 1024 * 1024);
    throws(() => new Blob(new Array(9).fill(part)), {
      name: 'RangeError',
      message: 'Blob size 150994944 exceeds limit 134217728',
    });
  },
};

export const resizableBuffers = {
  test() {
    const parts = () => [
      new ArrayBuffer(1, { maxByteLength: 2 }),
      new Uint8Array(new ArrayBuffer(1, { maxByteLength: 2 })),
      new DataView(new ArrayBuffer(1, { maxByteLength: 2 })),
      new Uint8Array(new SharedArrayBuffer(1, { maxByteLength: 2 })),
    ];
    if (!rejectResizable) {
      strictEqual(new Blob(parts()).size, 4);
      return;
    }
    const rejected = {
      name: 'TypeError',
      message: 'Cannot create a Blob with a resizable ArrayBuffer',
    };
    for (const part of parts()) {
      throws(() => new Blob([part]), rejected);
    }
    // A growable SharedArrayBuffer itself is not a BufferSource, so it is
    // converted to a string rather than rejected.
    strictEqual(
      new Blob([new SharedArrayBuffer(1, { maxByteLength: 2 })]).size,
      26
    );
    // Rejected only once every part is converted.
    const log = [];
    throws(
      () =>
        new Blob([
          new ArrayBuffer(1, { maxByteLength: 2 }),
          {
            toString() {
              log.push('converted');
              return 'x';
            },
          },
        ]),
      rejected
    );
    deepStrictEqual(log, ['converted']);
  },
};
