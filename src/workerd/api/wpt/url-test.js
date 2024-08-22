import { strictEqual } from 'assert';

// The harness has no exports of it's own but does modify the global
// scope to include the additional stuff the Web Platform Tests expect.
import * as harness from 'harness';

export const test = {
  async test() {
    // The tests will be run when the module is imported. Note that
    // there are limitations to this in that the module is not allowed
    // to perform any I/O... so any test that requires setTimeout,
    // for instance, will fail... this also means we cannot run
    // fetch tests from WPT... sad face.
    const foo = await import('url-origin.any.js');

    if (globalThis.errors.length > 0) {
      for (const err of globalThis.errors) {
        console.error(err);
      }
      throw new Error('Test failed');
    }
  },
};
