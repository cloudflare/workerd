import { strictEqual } from 'node:assert';
import { stripTypeScriptTypes } from 'node:module';

export const typeStripTest = {
  async test() {
    const source = 'function foo(x: number): number { return x + 1; }';
    const check = 'function foo(x        )         { return x + 1; }';
    const result = stripTypeScriptTypes(source);
    strictEqual(result, check);
    strictEqual(result.length, source.length);
  },
};
