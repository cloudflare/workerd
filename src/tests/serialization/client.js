// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* global CloneableTestBox */

import assert from 'node:assert';
import { RpcTarget } from 'cloudflare:workers';

const SERVERS = ['SERVER_CPP_STREAMS', 'SERVER_TS_STREAMS'];

function assertBox(value, label, contents) {
  assert.ok(value instanceof CloneableTestBox);
  assert.strictEqual(Object.getPrototypeOf(value), CloneableTestBox.prototype);
  assert.strictEqual(value.label, label);
  assert.deepStrictEqual(value.value, contents);
}

export const argument = {
  async test(ctrl, env) {
    for (const server of SERVERS) {
      const result = await env[server].describe(
        new CloneableTestBox('argument', { n: 1 })
      );
      assert.deepStrictEqual(result, {
        isBox: true,
        label: 'argument',
        value: { n: 1 },
      });
    }
  },
};

export const returnValue = {
  async test(ctrl, env) {
    for (const server of SERVERS) {
      assertBox(
        await env[server].makeBox('returned', [1, 2]),
        'returned',
        [1, 2]
      );
    }
  },
};

export const roundTrip = {
  async test(ctrl, env) {
    for (const server of SERVERS) {
      // Boxes nested in boxes, and one that appears twice, which arrives as
      // one object.
      const inner = new CloneableTestBox('inner', 'x');
      const outer = new CloneableTestBox('outer', [inner, inner]);
      const copy = await env[server].echo(outer);
      assert.ok(copy instanceof CloneableTestBox);
      assert.strictEqual(copy.label, 'outer');
      const [a, b] = copy.value;
      assertBox(a, 'inner', 'x');
      assert.strictEqual(a, b);
    }
  },
};

// Under enhanced_error_serialization, a thrown value reaches the caller as
// itself, not only an Error describing it.
export const thrown = {
  async test(ctrl, env) {
    for (const server of SERVERS) {
      await assert.rejects(env[server].throwBox('thrown'), (error) => {
        assertBox(error, 'thrown', { thrown: true });
        return true;
      });
    }
  },
};

class Greeter extends RpcTarget {
  hello() {
    return 'hi';
  }
}

export const externalsInData = {
  async test(ctrl, env) {
    for (const server of SERVERS) {
      const box = new CloneableTestBox('externals', {
        fn: () => 42,
        target: new Greeter(),
        stream: new Blob(['hello']).stream(),
      });
      assert.deepStrictEqual(await env[server].useValue(box), [
        42,
        'hi',
        'hello',
      ]);
    }
  },
};

// A worker without the flag that exposes CloneableTestBox doesn't load its
// module, so it can't be made to create one, even by a peer that has it.
export const unexposedClass = {
  async test(ctrl, env) {
    await assert.rejects(
      env.SERVER_WITHOUT_TEST_CLASS.describe(new CloneableTestBox('box', 1)),
      {
        name: 'DataCloneError',
        message:
          'Deserialization failed: unknown class implemented in JavaScript.',
      }
    );
  },
};

// A value whose instance data holds an external can't be cloned outside
// RPC.
export const externalsOutsideRpc = {
  test() {
    assert.throws(() => structuredClone(new CloneableTestBox('fn', () => 1)), {
      name: 'DataCloneError',
    });
  },
};

// Validation rejects data no instance would have.
export const constructorValidation = {
  test() {
    assert.throws(() => new CloneableTestBox(1), {
      name: 'TypeError',
      message: 'CloneableTestBox label must be a string',
    });
  },
};
