import assert from 'node:assert';
import module from 'node:module';

export const getBuiltinModule = {
  async test() {
    assert.deepStrictEqual(
      process.getBuiltinModule('node:sys'),
      process.getBuiltinModule('node:util')
    );
  },
};

export const canBeRequired = {
  async test() {
    const require = module.createRequire('/hello');
    assert.deepStrictEqual(require('node:sys'), require('node:util'));
    assert.deepStrictEqual(require('sys'), require('node:util'));
  },
};

export const canBeImported = {
  async test() {
    const sys = await import('node:sys');
    const util = await import('node:util');
    assert.deepStrictEqual(sys, util);
  },
};
