import assert from 'node:assert';

export const processPlatform = {
  test() {
    assert.strictEqual(typeof process.platform, 'string');
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
