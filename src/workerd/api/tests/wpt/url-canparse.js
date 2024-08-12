import { strictEqual } from 'node:assert'

export const testWpt = {
  async test(ctx) {
    await import('./wpt')
    strictEqual(ctx, '');
  }
}
