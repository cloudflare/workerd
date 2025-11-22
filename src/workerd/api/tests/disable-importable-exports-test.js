import { exports, withExports } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';

// This test runs with the disallow-importable-env flag set, which should also
// disable importable exports. The exports imported from cloudflare:workers
// should not be populated. Using withExports, however, would still work.

export class MyService {
  greet(name) {
    return `Hello, ${name}!`;
  }
}

export const test = {
  async test() {
    // exports should be empty/undefined when disallow_importable_env is set
    strictEqual(exports.MyService, undefined);
    strictEqual(exports.test, undefined);

    // withExports still works as expected with an object
    const result = await withExports({ customExport: 'test' }, async () => {
      await scheduler.wait(0);
      const { exports: otherExports } = await import('child');
      strictEqual(otherExports.customExport, 'test');
      strictEqual(otherExports.MyService, undefined);
      return otherExports.customExport;
    });
    strictEqual(result, 'test');

    // withExports with non-object values should fall back to disabled behavior
    const nonObjectValues = [null, 'string', 123, undefined];
    for (const value of nonObjectValues) {
      await withExports(value, async () => {
        await scheduler.wait(0);
        // When withExports gets a non-object, getCurrentExports falls through to the
        // base behavior (which is disabled by disallow_importable_env), so exports are empty
        strictEqual(exports.MyService, undefined);
        strictEqual(exports.customExport, undefined);
      });
    }
  },
};
