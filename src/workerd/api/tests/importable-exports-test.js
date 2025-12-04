import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { DurableObject, exports, withExports } from 'cloudflare:workers';

export class MyService extends DurableObject {
  greet(name) {
    return `Hello, ${name}!`;
  }
}

export const importableExports = {
  async test() {
    ok(
      exports.importableExports,
      'exports.importableExports should be defined'
    );
    ok(exports.nullableExports, 'exports.nullableExports should be defined');
    ok(exports.MyService, 'exports.MyService should be defined');

    const keys = Object.keys(exports).sort();
    deepStrictEqual(keys, [
      'MyService',
      'importableExports',
      'nullableExports',
    ]);

    const id = exports.MyService.idFromName('test');
    const stub = exports.MyService.get(id);
    strictEqual(await stub.greet('World'), 'Hello, World!');

    await scheduler.wait(10);
    ok(
      exports.importableExports,
      'exports.importableExports should persist after async wait'
    );

    const customExports = { customValue: 'test', customNumber: 42 };
    const result = await withExports(customExports, async () => {
      await scheduler.wait(10);
      strictEqual(exports.customValue, 'test');
      strictEqual(exports.customNumber, 42);
      strictEqual(
        exports.importableExports,
        undefined,
        'base exports should be masked'
      );
      return exports.customValue;
    });
    strictEqual(result, 'test');

    ok(
      exports.importableExports,
      'exports should be restored after withExports'
    );
    strictEqual(exports.customValue, undefined);

    for (const value of ['string', 123]) {
      await withExports(value, async () => {
        await scheduler.wait(10);
        ok(
          exports.importableExports,
          'non-object values should fall back to base exports'
        );
      });
    }
  },
};

export const nullableExports = {
  async test() {
    await withExports(null, async () => {
      strictEqual(exports.importableExports, undefined);
      strictEqual(exports.nullableExports, undefined);
      strictEqual(typeof exports, 'object');
    });

    await withExports(undefined, async () => {
      strictEqual(exports.importableExports, undefined);
      strictEqual(exports.nullableExports, undefined);
      strictEqual(typeof exports, 'object');
    });

    ok(exports.importableExports, 'exports should be restored');
  },
};
