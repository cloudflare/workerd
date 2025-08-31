import { throws } from 'node:assert';
import { glob, globSync, promises } from 'node:fs';

function mustNotCall() {
  throw new Error('This function should not be called');
}

export const globTest = {
  test() {
    // Glob is unsupported currently in workerd.
    // Verify that we're throwing an error as expected.

    throws(() => glob('*.js', {}, mustNotCall), {
      code: 'ERR_UNSUPPORTED_OPERATION',
    });

    throws(() => globSync('*.js', {}), {
      code: 'ERR_UNSUPPORTED_OPERATION',
    });

    throws(() => promises.glob('*.js', {}), {
      code: 'ERR_UNSUPPORTED_OPERATION',
    });
  },
};
