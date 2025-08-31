import assert from 'node:assert';
import * as processMod from 'node:process';

// Test that when enable_nodejs_process_v2 is disabled, the process module
// only exports the legacy subset of properties: nextTick, env, exit,
// getBuiltinModule, platform, and features

const expectedLegacyKeys = [
  'default',
  'env',
  'exit',
  'features',
  'getBuiltinModule',
  'nextTick',
  'platform',
];

export const processLegacyNamedExports = {
  test() {
    // Test that named exports only include the legacy subset
    const actualKeys = Object.keys(processMod).sort();
    assert.deepStrictEqual(actualKeys, expectedLegacyKeys);
  },
};

export const processLegacyDefaultExport = {
  test() {
    // Test that default export only includes the legacy subset
    const actualKeys = Object.keys(processMod.default).sort();
    const expectedDefaultKeys = expectedLegacyKeys.filter(
      (key) => key !== 'default'
    );
    assert.deepStrictEqual(actualKeys, expectedDefaultKeys);
  },
};

export const processLegacyGlobalProcess = {
  test() {
    // Test that global process only includes the legacy subset
    const actualKeys = Object.keys(process)
      .filter((v) => v[0] !== '_') // Filter out private properties
      .sort();
    const expectedDefaultKeys = expectedLegacyKeys.filter(
      (key) => key !== 'default'
    );
    assert.deepStrictEqual(actualKeys, expectedDefaultKeys);
  },
};

export const processLegacyFunctionality = {
  async test() {
    // Test that all legacy properties are functional

    // Test nextTick
    let nextTickCalled = false;
    process.nextTick(() => {
      nextTickCalled = true;
    });
    await scheduler.wait(0);
    assert.ok(nextTickCalled);

    // Test env (should have FOO=BAR from binding)
    assert.strictEqual(process.env.FOO, 'BAR');
    assert.strictEqual(typeof process.env, 'object');

    // Test platform
    assert.strictEqual(typeof process.platform, 'string');
    assert.ok(['darwin', 'win32', 'linux'].includes(process.platform));

    // Test features
    assert.strictEqual(typeof process.features, 'object');
    assert.ok(process.features !== null);

    // Test getBuiltinModule
    assert.strictEqual(typeof process.getBuiltinModule, 'function');
    const processBuiltin = process.getBuiltinModule('node:process');
    assert.strictEqual(typeof processBuiltin, 'object');

    // Test exit function exists (but don't call it)
    assert.strictEqual(typeof process.exit, 'function');
  },
};

export const processLegacyNoV2Properties = {
  test() {
    // Test that v2-only properties are NOT present
    const v2OnlyProperties = [
      'version',
      'versions',
      'title',
      'argv',
      'argv0',
      'execArgv',
      'arch',
      'config',
      'pid',
      'ppid',
      'hrtime',
      'emitWarning',
      'abort',
      'uptime',
      'loadEnvFile',
      'getegid',
      'geteuid',
      'getgid',
      'getgroups',
      'getuid',
      'setegid',
      'seteuid',
      'setgid',
      'setgroups',
      'setuid',
      'initgroups',
      'setSourceMapsEnabled',
      'getSourceMapsSupport',
      'allowedNodeEnvironmentFlags',
    ];

    for (const prop of v2OnlyProperties) {
      assert.strictEqual(
        process[prop],
        undefined,
        `process.${prop} should be undefined in legacy mode`
      );
      assert.strictEqual(
        processMod[prop],
        undefined,
        `processMod.${prop} should be undefined in legacy mode`
      );
      assert.strictEqual(
        processMod.default[prop],
        undefined,
        `processMod.default.${prop} should be undefined in legacy mode`
      );
    }
  },
};

export const processLegacyConsistency = {
  test() {
    // Test that all three ways of accessing process are consistent
    const processKeys = Object.keys(process)
      .filter((k) => k[0] !== '_')
      .sort();
    const processModKeys = Object.keys(processMod)
      .filter((k) => k !== 'default')
      .sort();
    const processDefaultKeys = Object.keys(processMod.default).sort();

    assert.deepStrictEqual(processKeys, processDefaultKeys);
    assert.deepStrictEqual(processModKeys, processDefaultKeys);

    // Test that function references are the same
    assert.strictEqual(process.nextTick, processMod.nextTick);
    assert.strictEqual(process.nextTick, processMod.default.nextTick);
    assert.strictEqual(process.exit, processMod.exit);
    assert.strictEqual(process.exit, processMod.default.exit);
    assert.strictEqual(process.getBuiltinModule, processMod.getBuiltinModule);
    assert.strictEqual(
      process.getBuiltinModule,
      processMod.default.getBuiltinModule
    );
  },
};
