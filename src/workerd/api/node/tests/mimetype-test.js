import {
  deepStrictEqual,
  ok,
  strictEqual,
} from 'node:assert';

import { MIMEType } from 'node:util';


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
    strictEqual(mt.toString(), 'text/plain;charset=utf-8;boundary=foo');

    deepStrictEqual(Array.from(mt.params), [
      ['charset', 'utf-8'],
      ['boundary', 'foo']
    ]);

    deepStrictEqual(Array.from(mt.params.keys()), [
      'charset', 'boundary'
    ]);

    deepStrictEqual(Array.from(mt.params.values()), [
      'utf-8', 'foo'
    ]);

    mt.params.delete('boundary');
    ok(!mt.params.has('boundary'));
  }
};
