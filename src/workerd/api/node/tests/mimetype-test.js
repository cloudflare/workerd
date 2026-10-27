// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';

import { inspect, MIMEParams, MIMEType } from 'node:util';

const setToStringTag = !!Cloudflare.compatibilityFlags.set_tostring_tag;

export const test_ok = {
  test() {
    const mt = new MIMEType('TeXt/PlAiN; ChArsEt="utf-8"');
    strictEqual(mt.type, 'text');
    strictEqual(mt.subtype, 'plain');
    strictEqual(mt.essence, 'text/plain');
    strictEqual(mt.toString(), 'text/plain;charset=utf-8');
    strictEqual(JSON.stringify(mt), '"text/plain;charset=utf-8"');
    strictEqual(mt.params.get('charset'), 'utf-8');
    ok(mt.params.has('charset'));
    ok(!mt.params.has('boundary'));
    mt.params.set('boundary', 'foo');
    ok(mt.params.has('boundary'));
    strictEqual(mt.toString(), 'text/plain;charset=utf-8; boundary=foo');

    deepStrictEqual(Array.from(mt.params), [
      ['charset', 'utf-8'],
      ['boundary', 'foo'],
    ]);

    deepStrictEqual(Array.from(mt.params.keys()), ['charset', 'boundary']);

    deepStrictEqual(Array.from(mt.params.values()), ['utf-8', 'foo']);

    mt.params.delete('boundary');
    ok(!mt.params.has('boundary'));
  },
};

// [input, toString() or null if the input is rejected, parameters]
const parseCases = [
  ['text/plain\u00a0', null],
  ['\u3000text/plain', null],
  ['text/plain', 'text/plain', []],
  ['TeXt/PlAiN', 'text/plain', []],
  ['  text/plain  ', 'text/plain', []],
  ['\t\r\ntext/plain', 'text/plain', []],
  ['text/plain\f', null],
  ['\u000btext/plain', null],
  ['\ftext/plain', null],
  ['text / plain', null],
  ['text/ plain', null],
  ['text /plain', null],
  ['text/plain ;a=b', 'text/plain;a=b', [['a', 'b']]],
  ['', null],
  [' ', null],
  ['/', null],
  ['text', null],
  ['text/', null],
  ['/plain', null],
  ['text/plain;', 'text/plain', []],
  ['text/plain;;;', 'text/plain', []],
  ['te(xt/plain', null],
  ['text/pl@in', null],
  ['text/plain/extra', null],
  ['*/*', '*/*', []],
  ['text/*', 'text/*', []],
  [
    'text/plain;charset=utf-8',
    'text/plain;charset=utf-8',
    [['charset', 'utf-8']],
  ],
  [
    'text/plain;CHARSET=UTF-8',
    'text/plain;charset=UTF-8',
    [['charset', 'UTF-8']],
  ],
  ['text/plain; charset = utf-8', 'text/plain', []],
  [
    'text/plain;charset= utf-8 ',
    'text/plain;charset=" utf-8"',
    [['charset', ' utf-8']],
  ],
  [
    'text/plain;charset="utf-8"',
    'text/plain;charset=utf-8',
    [['charset', 'utf-8']],
  ],
  [
    'text/plain;charset="ut\\"f-8"',
    'text/plain;charset="ut\\"f-8"',
    [['charset', 'ut"f-8']],
  ],
  [
    'text/plain;charset="a\\\\b"',
    'text/plain;charset=a\\b',
    [['charset', 'a\\b']],
  ],
  [
    'text/plain;charset="abc\\',
    'text/plain;charset=abc\\',
    [['charset', 'abc\\']],
  ],
  ['text/plain;charset="abc', 'text/plain;charset=abc', [['charset', 'abc']]],
  [
    'text/plain;charset="abc" trailing;x=y',
    'text/plain;charset=abc;x=y',
    [
      ['charset', 'abc'],
      ['x', 'y'],
    ],
  ],
  [
    'text/plain;charset="a;b";x=y',
    'text/plain;charset="a;b";x=y',
    [
      ['charset', 'a;b'],
      ['x', 'y'],
    ],
  ],
  ['text/plain;charset=""', 'text/plain;charset=""', [['charset', '']]],
  ['text/plain;charset=', 'text/plain', []],
  ['text/plain;charset', 'text/plain', []],
  ['text/plain;=foo;a=b', 'text/plain;a=b', [['a', 'b']]],
  ['text/plain;a b=c;d=e', 'text/plain;d=e', [['d', 'e']]],
  ['text/plain;a=b c', 'text/plain;a="b c"', [['a', 'b c']]],
  ['text/plain;a=b;a=c', 'text/plain;a=b', [['a', 'b']]],
  ['text/plain;A=b;a=c', 'text/plain;a=b', [['a', 'b']]],
  [
    'text/plain;boundary=xyz;a=b',
    'text/plain; boundary=xyz;a=b',
    [
      ['boundary', 'xyz'],
      ['a', 'b'],
    ],
  ],
  [
    'text/plain;a=b;boundary=xyz',
    'text/plain;a=b; boundary=xyz',
    [
      ['a', 'b'],
      ['boundary', 'xyz'],
    ],
  ],
  ['text/plain;a="b c"', 'text/plain;a="b c"', [['a', 'b c']]],
  ['text/plain;a="b\\"c"', 'text/plain;a="b\\"c"', [['a', 'b"c']]],
  ['text/plain;a="b\\\\c"', 'text/plain;a=b\\c', [['a', 'b\\c']]],
  ['text/plain;a=b\u0000c', 'text/plain', []],
  ['text/plain;a="b\u0000c"', 'text/plain', []],
  ['text/plain;a=b\u007fc', 'text/plain', []],
  ['text/plain;a="b\tc"', 'text/plain;a="b\tc"', [['a', 'b\tc']]],
  ['text/plain;a=b\tc', 'text/plain;a="b\tc"', [['a', 'b\tc']]],
  ['text/plain;a=\u00e9', 'text/plain;a="\u00e9"', [['a', '\u00e9']]],
  ['text/plain;a="\u00e9"', 'text/plain;a="\u00e9"', [['a', '\u00e9']]],
  [
    'text/plain;a=\ud83d\ude00',
    'text/plain;a="\ud83d\ude00"',
    [['a', '\ud83d\ude00']],
  ],
  [
    'text/plain;a=\ud800',
    'text/plain;a="\ufffd\ufffd\ufffd"',
    [['a', '\ufffd\ufffd\ufffd']],
  ],
  [
    'text/plain;a="x\udc00y"',
    'text/plain;a="x\ufffd\ufffd\ufffdy"',
    [['a', 'x\ufffd\ufffd\ufffdy']],
  ],
  ['t\u00e9xt/plain', null],
  ['text/pl\u00e1in', null],
  ['text/plain;\u00e9=a', 'text/plain', []],
  ['text/plain;a=b;c', 'text/plain;a=b', [['a', 'b']]],
  [
    'text/plain;a=b;c;d=e',
    'text/plain;a=b;d=e',
    [
      ['a', 'b'],
      ['d', 'e'],
    ],
  ],
  ['text/plain; ; a=b', 'text/plain;a=b', [['a', 'b']]],
  ['text/plain;a=(b)', 'text/plain;a="(b)"', [['a', '(b)']]],
  ['text/plain;a="(b)"', 'text/plain;a="(b)"', [['a', '(b)']]],
  ['text/plain;a=b\\c', 'text/plain;a=b\\c', [['a', 'b\\c']]],
  ['text/plain;a="\\b"', 'text/plain;a=b', [['a', 'b']]],
  [
    'text/plain;a=",";b=c',
    'text/plain;a=",";b=c',
    [
      ['a', ','],
      ['b', 'c'],
    ],
  ],
  ['text/plain,text/html', null],
];

export const test_parse = {
  test() {
    for (const [input, expected, params] of parseCases) {
      const label = JSON.stringify(input);
      if (expected === null) {
        throws(
          () => new MIMEType(input),
          {
            name: 'TypeError',
            message: `Not a valid MIME type: ${input}`,
          },
          label
        );
        continue;
      }
      const mt = new MIMEType(input);
      strictEqual(mt.toString(), expected, label);
      strictEqual(mt.toJSON(), expected, label);
      strictEqual(`${mt.type}/${mt.subtype}`, mt.essence, label);
      deepStrictEqual([...mt.params], params, label);
    }
  },
};

export const test_lone_surrogates = {
  test() {
    // Each lone surrogate becomes three U+FFFD, the result of converting it to UTF-8 and back.
    deepStrictEqual(
      [...new MIMEType('a/b;x=\ud800;y="p\udc00q";z=\ud83d\ude00').params],
      [
        ['x', '\ufffd\ufffd\ufffd'],
        ['y', 'p\ufffd\ufffd\ufffdq'],
        ['z', '\ud83d\ude00'],
      ]
    );
    const mt = new MIMEType('a/b');
    mt.params.set('x', 'p\ud800');
    strictEqual(mt.params.get('x'), 'p\ufffd\ufffd\ufffd');
    throws(() => new MIMEType('\ud800'), {
      message: 'Not a valid MIME type: \ufffd\ufffd\ufffd',
    });
    // Replaced before parsing, so an escape between two lone surrogates doesn't join them.
    strictEqual(
      new MIMEType('a/b;x="\ud800\\\udc00"').params.get('x'),
      '\ufffd'.repeat(6)
    );
  },
};

export const test_arguments = {
  test() {
    strictEqual(new MIMEType({ toString: () => 'A/B' }).toString(), 'a/b');
    strictEqual(new MIMEType('a/b', 'c/d').toString(), 'a/b');
    throws(() => new MIMEType(undefined), {
      name: 'TypeError',
      message: 'Not a valid MIME type: undefined',
    });
    throws(() => new MIMEType(null), {
      name: 'TypeError',
      message: 'Not a valid MIME type: null',
    });
    throws(() => new MIMEType(123), {
      name: 'TypeError',
      message: 'Not a valid MIME type: 123',
    });
    throws(() => new MIMEType(), {
      name: 'TypeError',
      message:
        "Failed to construct 'MIMEType': constructor parameter 1 is not of type 'string'.",
    });
    throws(() => new MIMEType(Symbol('x')), {
      name: 'TypeError',
      message: 'Cannot convert a Symbol value to a string',
    });
    throws(
      () =>
        new MIMEType({
          toString() {
            throw new RangeError('boom');
          },
        }),
      { name: 'RangeError', message: 'boom' }
    );
    throws(() => MIMEType('a/b'), TypeError);
    throws(() => MIMEParams(), TypeError);

    const { params } = new MIMEType('a/b;undefined=1;1=2');
    strictEqual(params.get(undefined), '1');
    strictEqual(params.get(1), '2');
    for (const [method, index, args] of [
      ['get', 1, []],
      ['has', 1, []],
      ['delete', 1, []],
      ['set', 1, []],
      ['set', 2, ['x']],
    ]) {
      throws(() => params[method](...args), {
        name: 'TypeError',
        message: `Failed to execute '${method}' on 'MIMEParams': parameter ${index} is not of type 'string'.`,
      });
    }
    params.set(3, null);
    strictEqual(params.get('3'), 'null');

    // Arguments are converted in order, before a missing one is reported.
    const converted = [];
    const arg = (name) => ({
      toString() {
        converted.push(name);
        return name;
      },
    });
    throws(() => params.set(arg('first')), TypeError);
    throws(() => new MIMEParams().set(arg('second')), TypeError);
    deepStrictEqual(converted, ['first', 'second']);
  },
};

export const test_setters = {
  test() {
    const mt = new MIMEType('text/plain;a=b');
    mt.type = 'Image';
    mt.subtype = 'PNG';
    strictEqual(mt.toString(), 'image/png;a=b');
    mt.type = 5;
    mt.subtype = null;
    strictEqual(mt.toString(), '5/null;a=b');
    mt.type = '*';
    strictEqual(mt.essence, '*/null');

    for (const value of ['', 'a b', 'a/b', ' html ', '\u00e9']) {
      throws(() => (mt.type = value), {
        name: 'TypeError',
        message: 'Not a valid MIME type',
      });
      throws(() => (mt.subtype = value), {
        name: 'TypeError',
        message: 'Not a valid MIME subtype',
      });
    }
    throws(() => (mt.type = Symbol('s')), {
      message: 'Cannot convert a Symbol value to a string',
    });
    strictEqual(mt.toString(), '*/null;a=b');

    throws(() => (mt.essence = 'a/b'), TypeError);
    throws(() => (mt.params = null), TypeError);

    // Freezing doesn't affect the internal state.
    Object.freeze(mt);
    mt.type = 'c';
    strictEqual(mt.type, 'c');
  },
};

export const test_params = {
  test() {
    const mt = new MIMEType('a/b;charset=x');
    const { params } = mt;
    strictEqual(mt.params, params);

    // get() and has() match the name exactly; missing parameters are null.
    strictEqual(params.get('charset'), 'x');
    strictEqual(params.get('CHARSET'), null);
    strictEqual(params.get('nope'), null);
    ok(!params.has('Charset'));

    // set() lowercases the name and doesn't replace an existing value.
    strictEqual(params.set('Y', 'Val'), undefined);
    params.set('charset', 'z');
    params.set('CHARSET', 'z');
    deepStrictEqual(
      [...params],
      [
        ['charset', 'x'],
        ['y', 'Val'],
      ]
    );

    // Values are quoted when they aren't tokens.
    const values = new MIMEType('a/b').params;
    values.set('empty', '');
    values.set('space', 'a b');
    values.set('escape', 'a"b\\c');
    values.set('semicolon', 'a;b');
    values.set('tab', 'a\tb');
    values.set('unicode', '\u00e9\ud83d\ude00');
    values.set('boundary', 'q');
    strictEqual(
      values.toString(),
      'empty="";space="a b";escape="a\\"b\\\\c";semicolon="a;b";tab="a\tb";' +
        'unicode="\u00e9\ud83d\ude00"; boundary=q'
    );
    strictEqual(values.toJSON(), values.toString());
    strictEqual(JSON.stringify(values), JSON.stringify(values.toString()));
    strictEqual(String(values), values.toString());

    // An invalid value is rejected even for an existing parameter.
    throws(() => params.set('charset', 'a\u0000'), {
      message: 'Not a valid MIME parameter',
    });

    for (const [name, value] of [
      ['y', 'a\u0000b'],
      ['y', 'a\u0001b'],
      ['y', 'a\u007fb'],
      ['a b', 'c'],
      ['', 'c'],
      ['\u00e9', 'c'],
    ]) {
      throws(() => values.set(name, value), {
        name: 'TypeError',
        message: 'Not a valid MIME parameter',
      });
    }
  },
};

export const test_params_delete = {
  test() {
    const order = (src, fn) => {
      const mt = new MIMEType(src);
      fn(mt.params);
      return mt.toString();
    };
    // Deleting a parameter moves the last one into its place.
    strictEqual(
      order('a/b;x=1;y=2;z=3', (p) => p.delete('x')),
      'a/b;z=3;y=2'
    );
    strictEqual(
      order('a/b;w=0;x=1;y=2;z=3', (p) => p.delete('x')),
      'a/b;w=0;z=3;y=2'
    );
    strictEqual(
      order('a/b;w=0;x=1;y=2;z=3', (p) => {
        p.delete('w');
        p.delete('y');
      }),
      'a/b;z=3;x=1'
    );
    strictEqual(
      order('a/b;x=1;y=2;z=3', (p) => {
        p.delete('x');
        p.set('w', '4');
      }),
      'a/b;z=3;y=2;w=4'
    );
    // delete() lowercases the name.
    strictEqual(
      order('a/b;x=1;y=2', (p) => p.delete('X')),
      'a/b;y=2'
    );
    strictEqual(
      order('a/b;x=1', (p) => p.delete('q')),
      'a/b;x=1'
    );
    // Only ASCII letters are lowercased.
    strictEqual(
      order('a/b;k=1', (p) => p.delete('\u212a')),
      'a/b;k=1'
    );
  },
};

export const test_params_iterators = {
  test() {
    const { params } = new MIMEType('a/b;x=1;y=2');

    // Iterators see the parameters as they were when they were created.
    const entries = params.entries();
    params.set('z', '3');
    params.delete('x');
    deepStrictEqual(
      [...entries],
      [
        ['x', '1'],
        ['y', '2'],
      ]
    );

    const keys = params.keys();
    deepStrictEqual(keys.next(), { done: false, value: 'z' });
    deepStrictEqual(keys.next(), { done: false, value: 'y' });
    // Without enable_fast_jsg_struct, the final result has no value property.
    const finalKeys = Cloudflare.compatibilityFlags.enable_fast_jsg_struct
      ? ['done', 'value']
      : ['done'];
    deepStrictEqual(Object.keys(keys.next()).sort(), finalKeys);
    deepStrictEqual(Object.keys(keys.next()).sort(), finalKeys);
    strictEqual(keys[Symbol.iterator](), keys);
    deepStrictEqual([...params.values()], ['3', '2']);

    // Each entry is a new array.
    params.entries().next().value[0] = 'changed';
    deepStrictEqual([...params.keys()], ['z', 'y']);

    // The iterators inherit the iterator helpers.
    deepStrictEqual(params.keys().toArray(), ['z', 'y']);
    for (const iterator of [params.entries(), params.keys(), params.values()]) {
      strictEqual(
        Object.getPrototypeOf(Object.getPrototypeOf(iterator)),
        Iterator.prototype
      );
      throws(() => new iterator.constructor([]), TypeError);
    }
    // next() only works on an iterator of its own kind.
    throws(
      () => Object.getPrototypeOf(params.keys()).next.call(params.entries()),
      { message: /^Illegal invocation/ }
    );

    const names = setToStringTag
      ? ['EntryIterator', 'KeyIterator', 'ValueIterator']
      : ['Iterator', 'Iterator', 'Iterator'];
    deepStrictEqual(
      [params.entries(), params.keys(), params.values()].map((i) =>
        Object.prototype.toString.call(i)
      ),
      names.map((name) => `[object ${name}]`)
    );

    strictEqual(typeof params.forEach, 'undefined');
    strictEqual(params.size, undefined);
  },
};

export const test_standalone_params = {
  test() {
    // A MIMEParams that doesn't belong to a MIMEType ignores every operation.
    const params = new MIMEParams('a=b');
    params.set('a', 'b');
    params.set('a b', 'c');
    params.delete('a');
    strictEqual(params.get('a'), null);
    ok(!params.has('a'));
    strictEqual(params.toString(), '');
    strictEqual(params.toJSON(), '');
    deepStrictEqual([...params], []);
    deepStrictEqual([...params.keys()], []);
    throws(() => params.set('a'), TypeError);

    class Sub extends MIMEParams {}
    ok(new Sub() instanceof MIMEParams);
  },
};

export const test_shape = {
  test() {
    const mt = new MIMEType('text/plain;charset=utf-8;boundary=x');
    ok(mt instanceof MIMEType);
    ok(mt.params instanceof MIMEParams);
    strictEqual(mt.params.constructor, MIMEParams);
    deepStrictEqual(Reflect.ownKeys(mt), []);
    deepStrictEqual(Reflect.ownKeys(mt.params), []);
    strictEqual(MIMEType.name, 'MIMEType');
    strictEqual(MIMEParams.name, 'MIMEParams');

    strictEqual(`${mt}`, 'text/plain;charset=utf-8; boundary=x');
    strictEqual(
      JSON.stringify({ mt }),
      '{"mt":"text/plain;charset=utf-8; boundary=x"}'
    );

    strictEqual(
      Object.prototype.toString.call(mt),
      setToStringTag ? '[object MIMEType]' : '[object Object]'
    );
    strictEqual(
      Object.prototype.toString.call(mt.params),
      setToStringTag ? '[object MIMEParams]' : '[object Object]'
    );

    strictEqual(
      inspect(mt),
      "MIMEType {\n  type: 'text',\n  subtype: 'plain',\n  essence: 'text/plain',\n" +
        "  params: MIMEParams(2) { 'charset' => 'utf-8', 'boundary' => 'x' }\n}"
    );
    strictEqual(inspect(new MIMEParams()), 'MIMEParams(0) {}');
    strictEqual(inspect({ mt }, { depth: 0 }), '{ mt: [MIMEType] }');
    strictEqual(
      inspect(mt, { depth: 0 }),
      "MIMEType {\n  type: 'text',\n  subtype: 'plain',\n  essence: 'text/plain',\n" +
        '  params: [MIMEParams]\n}'
    );

    const internalClass = Symbol.for('cloudflare:internal-class');
    for (const proto of [
      MIMEType.prototype,
      MIMEParams.prototype,
      Object.getPrototypeOf(mt.params.entries()),
      Object.getPrototypeOf(mt.params.keys()),
      Object.getPrototypeOf(mt.params.values()),
    ]) {
      deepStrictEqual(Object.getOwnPropertyDescriptor(proto, internalClass), {
        value: internalClass,
        writable: false,
        enumerable: false,
        configurable: false,
      });
      const tag = Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag);
      if (setToStringTag) {
        deepStrictEqual(
          { ...tag, value: undefined },
          {
            value: undefined,
            writable: true,
            enumerable: false,
            configurable: true,
          }
        );
      } else {
        strictEqual(tag, undefined);
      }
    }
    strictEqual(inspect(mt.params.entries()), 'EntryIterator {}');

    class Sub extends MIMEType {
      extra() {
        return this.essence;
      }
    }
    const sub = new Sub('A/B');
    strictEqual(sub.extra(), 'a/b');
    ok(sub instanceof MIMEType);
  },
};

export const test_illegal_invocation = {
  test() {
    const error = {
      name: 'TypeError',
      message:
        /^Illegal invocation: function called with incorrect `this` reference\./,
    };
    const type = Object.getOwnPropertyDescriptor(MIMEType.prototype, 'type');
    throws(() => type.get.call({}), error);
    throws(() => type.set.call({}, 'a'), error);
    throws(() => MIMEType.prototype.type, error);
    throws(() => MIMEType.prototype.toString.call({}), error);
    throws(() => Object.create(MIMEType.prototype).toString(), error);
    throws(() => MIMEParams.prototype.get.call({}, 'a'), error);
    throws(() => MIMEParams.prototype.entries.call(new MIMEType('a/b')), error);
  },
};

export const test_ignores_patched_builtins = {
  test() {
    // The helpers here capture what they need and avoid the methods they patch.
    const OriginalTypeError = TypeError;
    const defineProperty = Object.defineProperty;
    const getOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
    const tampered = () => {
      throw new Error('tampered');
    };
    const saved = { __proto__: null };
    let count = 0;
    const patch = (target, key, descriptor) => {
      saved[count++] = {
        target,
        key,
        original: getOwnPropertyDescriptor(target, key),
      };
      defineProperty(target, key, { configurable: true, ...descriptor });
    };
    const patchMethods = (target, keys) => {
      for (let i = 0; i < keys.length; i++) {
        patch(target, keys[i], { value: tampered, writable: true });
      }
    };
    let parsed,
      type,
      lowered,
      params,
      got,
      missing,
      has,
      entry,
      key,
      value,
      done;
    let invalid, invalidSet, standalone;

    try {
      patchMethods(String.prototype, [
        'slice',
        'substring',
        'indexOf',
        'charAt',
        'charCodeAt',
        'codePointAt',
        'at',
        'toLowerCase',
        'replace',
        'replaceAll',
        'isWellFormed',
        'split',
        'trim',
        Symbol.iterator,
      ]);
      patchMethods(String, ['fromCharCode']);
      patchMethods(Map.prototype, ['get', 'set', 'has', 'delete']);
      patchMethods(WeakMap.prototype, ['get', 'set', 'has', 'delete']);
      patchMethods(Array.prototype, [
        'push',
        'pop',
        'map',
        'concat',
        'slice',
        'join',
        'flat',
        Symbol.iterator,
      ]);
      patchMethods(Object, ['create', 'defineProperty', 'freeze']);
      patch(globalThis, 'TypeError', { value: tampered, writable: true });
      // Setters reached by writes to missing properties. A 'value' accessor on Object.prototype
      // makes later accessor descriptors invalid, so it comes last.
      const indexKeys = ['0', '1', '2', '3'];
      for (let i = 0; i < indexKeys.length; i++) {
        patch(Array.prototype, indexKeys[i], { set: tampered });
        patch(Object.prototype, indexKeys[i], { set: tampered });
      }
      const objectKeys = ['done', 'length', 'items', 'value'];
      for (let i = 0; i < objectKeys.length; i++) {
        patch(Object.prototype, objectKeys[i], { set: tampered });
      }
      const mt = new MIMEType(
        'Text/HTML; Charset="UTF-8"; a="x\\"y"; b=c d;b=e'
      );
      parsed = mt.toString();
      mt.type = 'APPLICATION';
      type = mt.essence;
      params = mt.params;
      params.set('Boundary', 'q r');
      params.delete('A');
      lowered = params.toString();
      got = params.get('charset');
      missing = params.get('nope');
      has = params.has('b');
      const entries = params.entries();
      entry = entries.next().value;
      key = params.keys().next().value;
      value = params.values().next().value;
      const values = params.values();
      values.next();
      values.next();
      values.next();
      done = values.next();
      try {
        new MIMEType('nope');
      } catch (e) {
        invalid = e;
      }
      try {
        params.set('x', '\u0000');
      } catch (e) {
        invalidSet = e;
      }
      standalone = new MIMEParams().toString();
    } finally {
      for (let i = count - 1; i >= 0; i--) {
        const { target, key, original } = saved[i];
        // eslint-disable-next-line @typescript-eslint/no-dynamic-delete
        if (original === undefined) delete target[key];
        else defineProperty(target, key, original);
      }
    }

    strictEqual(parsed, 'text/html;charset=UTF-8;a="x\\"y";b="c d"');
    strictEqual(type, 'application/html');
    strictEqual(lowered, 'charset=UTF-8; boundary="q r";b="c d"');
    strictEqual(got, 'UTF-8');
    strictEqual(missing, null);
    strictEqual(has, true);
    deepStrictEqual(entry, ['charset', 'UTF-8']);
    strictEqual(key, 'charset');
    strictEqual(value, 'UTF-8');
    strictEqual(done.done, true);
    ok(invalid instanceof OriginalTypeError);
    strictEqual(invalid.message, 'Not a valid MIME type: nope');
    ok(invalidSet instanceof OriginalTypeError);
    strictEqual(invalidSet.message, 'Not a valid MIME parameter');
    strictEqual(standalone, '');
  },
};
