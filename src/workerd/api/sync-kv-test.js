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

    {
      let map = kv.list();
      assert.strictEqual(map.size, 2);
      assert.strictEqual(map.get('foo'), 123);
      assert.strictEqual(map.get('bar'), 'abc');

      // Iterates in alphabetical order.
      assert.deepEqual(
        [...map],
        [
          ['bar', 'abc'],
          ['foo', 123],
        ]
      );
    }

    {
      let map = kv.list({ reverse: true });
      assert.deepEqual(
        [...map],
        [
          ['foo', 123],
          ['bar', 'abc'],
        ]
      );
    }

    kv.put('baz', false);

    // Try a prefix.
    {
      let map = kv.list({ prefix: 'ba' });
      assert.strictEqual(map.size, 2);
      assert.strictEqual(map.has('foo'), false);
      assert.strictEqual(map.get('bar'), 'abc');
      assert.strictEqual(map.get('baz'), false);
      assert.deepEqual(
        [...map],
        [
          ['bar', 'abc'],
          ['baz', false],
        ]
      );
    }

    // Try a limit.
    {
      let map = kv.list({ limit: 1 });
      assert.deepEqual([...map], [['bar', 'abc']]);
    }

    // Try a reverse limit.
    {
      let map = kv.list({ limit: 1, reverse: true });
      assert.deepEqual([...map], [['foo', 123]]);
    }

    // Try a range.
    {
      let map = kv.list({ start: 'b', end: 'c' });
      assert.deepEqual(
        [...map],
        [
          ['bar', 'abc'],
          ['baz', false],
        ]
      );
    }

    // End is exclusive.
    {
      let map = kv.list({ start: 'b', end: 'baz' });
      assert.deepEqual([...map], [['bar', 'abc']]);
    }

    // Start is inclusive.
    {
      let map = kv.list({ start: 'bar', end: 'c' });
      assert.deepEqual(
        [...map],
        [
          ['bar', 'abc'],
          ['baz', false],
        ]
      );
    }

    // Except when it's not.
    {
      let map = kv.list({ startAfter: 'bar', end: 'c' });
      assert.deepEqual([...map], [['baz', false]]);
    }

    // Test multi-get.
    {
      let map = kv.get(['bar', 'foo']);
      assert.deepEqual(
        [...map],
        [
          ['bar', 'abc'],
          ['foo', 123],
        ]
      );
    }

    // Return iteration order is same as input order.
    // NOTE: This differs from the async interface, which always returned sorted results. Probably
    //   nobody cares, so I took the opportunity to remove the need for the sort.
    {
      let map = kv.get(['foo', 'bar']);
      assert.deepEqual(
        [...map],
        [
          ['foo', 123],
          ['bar', 'abc'],
        ]
      );
    }

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
