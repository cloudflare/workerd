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

    // withExports still works as expected though
    const result = await withExports({ customExport: 'test' }, async () => {
      await scheduler.wait(0);
      const { exports: otherExports } = await import('child');
      strictEqual(otherExports.customExport, 'test');
      strictEqual(otherExports.MyService, undefined);
      return otherExports.customExport;
    });
    strictEqual(result, 'test');
  },
};
