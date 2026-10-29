// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
const opens = [];
let outcomes = 0;
export default {
  tailStream(onset) {
    return (event) => {
      if (event.event.type === 'spanOpen') opens.push(event.event);
      else {
        assert.strictEqual(Object.hasOwn(event.event, 'kind'), false);
        assert.strictEqual(Object.hasOwn(event.event, 'scope'), false);
      }
      if (event.event.type === 'outcome') outcomes++;
    };
  },
};

export const metadata = {
  async test() {
    for (let i = 0; outcomes < 2 && i < 100; i++) await scheduler.wait(1);
    assert.strictEqual(outcomes, 2);
    const open = (name) => opens.find((event) => event.name === name);
    const scope = {
      name: 'metadata-test',
      version: '1.2.3',
      schemaUrl: 'https://example.com/schema',
    };
    for (const [kind, name] of [
      'internal',
      'server',
      'client',
      'producer',
      'consumer',
    ].entries()) {
      assert.strictEqual(open(`kind-${kind}`).kind, name);
      assert.deepStrictEqual(open(`kind-${kind}`).scope, scope);
    }
    assert.strictEqual(open('default-kind').kind, 'internal');
    assert.deepStrictEqual(open('default-kind').scope, scope);
    assert.deepStrictEqual(open('name-only').scope, { name: 'name-only' });
    assert.deepStrictEqual(open('other-version').scope, {
      name: 'metadata-test',
      version: '2.0',
    });
    assert.deepStrictEqual(open('empty-optionals').scope, {
      name: 'empty',
      version: '',
      schemaUrl: '',
    });
    for (const enabled of [false, true]) {
      assert.deepStrictEqual(Object.keys(open(`native-${enabled}`)), [
        'type',
        'name',
        'spanId',
      ]);
    }
    assert.strictEqual(opens.length, 11);
  },
};
