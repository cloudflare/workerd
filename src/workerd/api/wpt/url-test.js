import * as harness from 'harness';

export const urlConstructor = {
  async test() {
    harness.prepare();
    await import('url-constructor.any.js');
    harness.validate();
  },
};

export const urlOrigin = {
  async test() {
    harness.prepare();
    await import('url-origin.any.js');
    harness.validate();
  },
};
