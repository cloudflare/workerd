import assert from 'node:assert';

export const processPlatform = {
  test() {
    assert.ok(['darwin', 'win32', 'linux'].includes(process.platform));
  },
};

process.env.QUX = 1;
const pEnv = { ...process.env };

export const processEnv = {
  async test(ctrl, env) {
    assert.notDeepStrictEqual(env, process.env);

    assert.strictEqual(pEnv.FOO, 'BAR');

    // It should be possible to mutate the process.env at runtime.
    // All values manually set are coerced to strings.
    assert.strictEqual(pEnv.QUX, '1');

    // JSON bindings that do not parse to strings come through as
    // raw unparsed JSON strings....
    assert.strictEqual(pEnv.BAR, '{}');
    JSON.parse(pEnv.BAR);

    // JSON bindings that parse as strings come through as the
    // parsed string value.
    assert.strictEqual(pEnv.BAZ, 'abc');

    // Throws because althought defined as a JSON binding, the value
    // that comes through to process.env in this case is not a JSON
    // parseable string because it will already have been parsed.
    // This assertion is only true when the JSON bindings value
    // happens to parse as a string.
    assert.throws(() => JSON.parse(pEnv.BAZ));

    // JSON bindings that parse as strings that happen to be double
    // escaped might be JSON parseable.
    assert.strictEqual(JSON.parse(pEnv.DUB), 'abc');

    // Test that imports can see the process.env at the top level
    const { FOO } = await import('mod');
    assert.strictEqual(FOO, 'BAR');

    // Mutating the env argument does not change process.env, and
    // vis versa. The reason for this is that just mutations may
    // be entirely surprising and unexpected for users.
    assert.strictEqual(env.QUX, undefined);
    env.ZZZ = 'a';
    assert.strictEqual(process.env.ZZZ, undefined);

    env.FOO = ['just some other value'];
    assert.strictEqual(process.env.FOO, 'BAR');

    // Other kinds of bindings will be on env but not process.env
    assert.strictEqual(pEnv.NON, undefined);
    assert.deepStrictEqual(
      new Uint8Array(env.NON),
      new Uint8Array([97, 98, 99, 100, 101, 102])
    );
  },
};

export const processProperties = {
  test() {
    // Test basic process properties
    assert.strictEqual(typeof process.title, 'string');
    assert.ok(process.title.length > 0);

    assert.ok(Array.isArray(process.argv));
    assert.ok(process.argv.length >= 1);

    assert.strictEqual(typeof process.arch, 'string');
    assert.strictEqual(process.arch, 'x64');

    assert.strictEqual(typeof process.version, 'string');
    assert.ok(process.version.startsWith('v'));

    // Test additional properties
    assert.strictEqual(process.argv0, 'workerd');
    assert.ok(Array.isArray(process.execArgv));
    assert.strictEqual(process.execArgv.length, 0);

    assert.strictEqual(process.pid, 1);
    assert.strictEqual(process.ppid, 0);

    // Test config object
    assert.strictEqual(typeof process.config, 'object');
    assert.strictEqual(typeof process.config.target_defaults, 'object');
    assert.strictEqual(typeof process.config.variables, 'object');

    // Test uid/gid functions
    assert.strictEqual(process.getegid(), 0);
    assert.strictEqual(process.geteuid(), 0);
    process.setegid(1000); // Should be no-op
    process.seteuid(1000); // Should be no-op
    assert.strictEqual(process.getegid(), 0);
    assert.strictEqual(process.geteuid(), 0);

    // Test other functions
    process.setSourceMapsEnabled(false); // Should be no-op

    // Test allowedNodeEnvironmentFlags
    assert.ok(process.allowedNodeEnvironmentFlags instanceof Set);
    assert.strictEqual(process.allowedNodeEnvironmentFlags.size, 0);
  },
};

export const processVersions = {
  test() {
    assert.strictEqual(typeof process.versions, 'object');
    assert.ok(process.versions !== null);

    const expectedVersions = ['node'];

    assert.strictEqual(
      Object.keys(expectedVersions).length,
      expectedVersions.length
    );

    // Check that all expected versions are included and are strings
    for (const versionKey of expectedVersions) {
      assert.strictEqual(
        typeof process.versions[versionKey],
        'string',
        `process.versions.${versionKey} should be a string`
      );
    }
  },
};

export const processFeatures = {
  test() {
    assert.strictEqual(typeof process.features, 'object');
    assert.ok(process.features !== null);

    // Process features should be an object that can be inspected
    const features = Object.keys(process.features);
    assert.ok(Array.isArray(features));
  },
};

export const processNextTick = {
  async test() {
    let called = false;
    let order = [];

    // Test basic nextTick functionality
    process.nextTick(() => {
      called = true;
      order.push('nextTick1');
    });

    // Test multiple nextTick calls
    process.nextTick(() => {
      order.push('nextTick2');
    });

    // Test nextTick with arguments
    process.nextTick(
      (arg1, arg2) => {
        assert.strictEqual(arg1, 'hello');
        assert.strictEqual(arg2, 'world');
        order.push('nextTick3');
      },
      'hello',
      'world'
    );

    order.push('sync');

    // Wait for microtasks to complete
    await new Promise((resolve) => setTimeout(resolve, 0));

    assert.ok(called);
    assert.deepStrictEqual(order, [
      'sync',
      'nextTick1',
      'nextTick2',
      'nextTick3',
    ]);
  },
};

export const processGetBuiltinModule = {
  test() {
    // Should be able to get built-in modules
    const processBuiltin = process.getBuiltinModule('node:process');
    assert.strictEqual(typeof processBuiltin, 'object');

    // Should return null/undefined for non-existent modules
    const nonExistent = process.getBuiltinModule('node:nonexistent');
    assert.ok(nonExistent == null);
  },
};
