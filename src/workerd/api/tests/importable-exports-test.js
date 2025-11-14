import { strictEqual, ok } from 'node:assert';
import { exports } from 'cloudflare:workers';

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
    ok(
      exports.MyService,
      'exports.MyService should still be defined after async wait'
    );
  },
};
