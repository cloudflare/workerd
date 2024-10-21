import assert from 'node:assert';

export const processPlatform = {
  test() {
    assert.strictEqual(typeof process.platform, 'string');
    assert.ok(['darwin', 'win32', 'linux'].includes(process.platform));
  },
};
