import { strictEqual } from 'node:assert';

export default {
  test() {
    const h = new Headers();
    strictEqual(h[Symbol.toStringTag], undefined);
  },
};
