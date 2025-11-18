import { strictEqual, ok } from 'node:assert';
import { exports, withExports } from 'cloudflare:workers';

export class MyService {
  greet(name) {
    return `Hello, ${name}!`;
  }
}

export const importableExports = {
  async test() {
    // Verify that exports are accessible
    ok(exports.MyService, 'exports.MyService should be defined');
    strictEqual(
      typeof exports.MyService,
      'function',
      'MyService should be a function'
    );
    ok(
      exports.importableExports,
      'exports.importableExports should be defined'
    );

    // Create an instance and test it
    const service = new exports.MyService();
    const result = service.greet('World');
    strictEqual(result, 'Hello, World!');

    // Following async operations, exports should still be accessible
    await scheduler.wait(10);
    strictEqual(
      exports.MyService,
      MyService,
      'exports.MyService should still be the same reference after async wait'
    );

    // Test withExports with an object value
    const customExports = { customValue: 'test', customNumber: 42 };
    const result2 = await withExports(customExports, async () => {
      await scheduler.wait(10);
      // Inside withExports, the exports should be overridden
      strictEqual(exports.customValue, 'test');
      strictEqual(exports.customNumber, 42);
      strictEqual(exports.MyService, undefined);
      return exports.customValue;
    });
    strictEqual(result2, 'test');

    // After withExports, exports should revert to base exports
    strictEqual(exports.MyService, MyService);
    strictEqual(exports.customValue, undefined);

    // Test withExports with non-object values - should fall back to base exports
    const nonObjectValues = [null, 'string', 123, undefined];
    for (const value of nonObjectValues) {
      await withExports(value, async () => {
        await scheduler.wait(10);
        // With non-object values, should fall back to base exports
        strictEqual(exports.MyService, MyService);
        strictEqual(
          exports.importableExports,
          importableExports,
          'Should still have base exports with non-object withExports value'
        );
      });
    }
  },
};
