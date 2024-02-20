import * as assert from 'a/b/c';
import * as assert3 from 'node:assert';

export const basics = {
  async test() {
    const assert2 = await import('a/b/c');
    if (assert !== assert2 && assert !== assert3) {
      throw new Error('bad things happened');
    }
  }
};
