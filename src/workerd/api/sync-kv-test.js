import { DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

export class MyActor extends DurableObject {
  run() {
    let kv = this.ctx.storage.kv;

    assert.strictEqual(kv.get('foo'), undefined);
    kv.put('foo', 123);
    assert.strictEqual(kv.get('foo'), 123);

    assert.strictEqual(kv.get('bar'), undefined);
    kv.put('bar', 'abc');
    assert.strictEqual(kv.get('bar'), 'abc');

    // Iterates in alphabetical order.
    assert.deepEqual(
      [...kv.list()],
      [
        ['bar', 'abc'],
        ['foo', 123],
      ]
    );

    assert.deepEqual(
      [...kv.list({ reverse: true })],
      [
        ['foo', 123],
        ['bar', 'abc'],
      ]
    );

    // A new call to kv.list() invalidates any previous iterator.
    {
      let cursor1 = kv.list();
      let cursor2 = kv.list();

      assert.throws(() => [...cursor1], {
        name: 'Error',
        message:
          'kv.list() iterator was invalidated because a new call to kv.list() was sarted. ' +
          'Only one kv.list() iterator can exist at a time.',
      });

      assert.deepEqual(
        [...cursor2],
        [
          ['bar', 'abc'],
          ['foo', 123],
        ]
      );
    }

    kv.put('baz', false);

    // Try a prefix.
    assert.deepEqual(
      [...kv.list({ prefix: 'ba' })],
      [
        ['bar', 'abc'],
        ['baz', false],
      ]
    );

    // Try a limit.
    assert.deepEqual([...kv.list({ limit: 1 })], [['bar', 'abc']]);

    // Try a reverse limit.
    assert.deepEqual([...kv.list({ limit: 1, reverse: true })], [['foo', 123]]);

    // Try a range.
    assert.deepEqual(
      [...kv.list({ start: 'b', end: 'c' })],
      [
        ['bar', 'abc'],
        ['baz', false],
      ]
    );

    // End is exclusive.
    assert.deepEqual(
      [...kv.list({ start: 'b', end: 'baz' })],
      [['bar', 'abc']]
    );

    // Start is inclusive.
    assert.deepEqual(
      [...kv.list({ start: 'bar', end: 'c' })],
      [
        ['bar', 'abc'],
        ['baz', false],
      ]
    );

    // Except when it's not.
    assert.deepEqual(
      [...kv.list({ startAfter: 'bar', end: 'c' })],
      [['baz', false]]
    );

    // Test multi-get.
    assert.deepEqual(
      [...kv.get(['bar', 'foo'])],
      [
        ['bar', 'abc'],
        ['foo', 123],
      ]
    );

    // Return iteration order is same as input order.
    // NOTE: This differs from the async interface, which always returned sorted results. Probably
    //   nobody cares, so I took the opportunity to remove the need for the sort.
    assert.deepEqual(
      [...kv.get(['foo', 'bar'])],
      [
        ['foo', 123],
        ['bar', 'abc'],
      ]
    );

    // Test delete.
    assert.strictEqual(kv.delete('qux'), false);
    assert.strictEqual(kv.delete('bar'), true);
    assert.deepEqual(
      [...kv.list()],
      [
        ['baz', false],
        ['foo', 123],
      ]
    );

    // Multi-delete.
    assert.strictEqual(kv.delete(['foo', 'bar', 'baz']), 2);
    assert.deepEqual([...kv.list()], []);
  }
}

export let testAutoRollBackOnCriticalError = {
  async test(ctrl, env, ctx) {
    await ctx.exports.MyActor.getByName('foo').run();
  },
};
