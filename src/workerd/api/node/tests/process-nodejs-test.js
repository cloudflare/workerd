import assert from 'node:assert';
import { writeFileSync } from 'node:fs';
import * as processMod from 'node:process';

export const processPlatform = {
  test() {
    assert.ok(['darwin', 'win32', 'linux'].includes(process.platform));
  },
};

process.env.QUX = 1;
const pEnv = { ...process.env };

// Verify process keys match default keys and features
const keys = [
  'abort',
  'allowedNodeEnvironmentFlags',
  'arch',
  'argv',
  'argv0',
  'binding',
  'channel',
  'chdir',
  'config',
  'connected',
  'cwd',
  'debugPort',
  'default',
  'dlopen',
  'emitWarning',
  'env',
  'execArgv',
  'exit',
  'exitCode',
  'features',
  'finalization',
  'getActiveResourcesInfo',
  'getBuiltinModule',
  'getSourceMapsSupport',
  'getegid',
  'geteuid',
  'getgid',
  'getgroups',
  'getuid',
  'hasUncaughtExceptionCaptureCallback',
  'hrtime',
  'initgroups',
  'kill',
  'loadEnvFile',
  'memoryUsage',
  'nextTick',
  'noDeprecation',
  'permission',
  'pid',
  'platform',
  'ppid',
  'ref',
  'release',
  'report',
  'resourceUsage',
  'send',
  'setSourceMapsEnabled',
  'setUncaughtExceptionCaptureCallback',
  'setegid',
  'seteuid',
  'setgid',
  'setgroups',
  'setuid',
  'sourceMapsEnabled',
  'stderr',
  'stdin',
  'stdout',
  'threadCpuUsage',
  'throwDeprecation',
  'title',
  'traceDeprecation',
  'umask',
  'unref',
  'uptime',
  'version',
  'versions',
];
export const processKeys = {
  test() {
    assert.deepStrictEqual(Object.keys(processMod), keys);
    keys.splice(keys.indexOf('default'), 1);
    assert.deepStrictEqual(
      Object.keys(process)
        .filter((v) => v[0] !== '_')
        .sort(),
      keys
    );
    assert.deepStrictEqual(
      Object.keys(processMod.default)
        .filter((v) => v[0] !== '_')
        .sort(),
      keys
    );
    const processBuiltin = processMod.getBuiltinModule('process');
    assert.deepStrictEqual(
      Object.keys(processBuiltin)
        .filter((v) => v[0] !== '_')
        .sort(),
      keys
    );
    const processBuiltinScheme = processMod.getBuiltinModule('node:process');
    assert.deepStrictEqual(
      Object.keys(processBuiltinScheme)
        .filter((v) => v[0] !== '_')
        .sort(),
      keys
    );
  },
};

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

    // Test process bindings delete https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-env-delete.js
    {
      process.env.foo = 'foo';
      assert.strictEqual(process.env.foo, 'foo');
      process.env.foo = undefined;
      assert.strictEqual(process.env.foo, 'undefined');

      process.env.foo = 'foo';
      assert.strictEqual(process.env.foo, 'foo');
      delete process.env.foo;
      assert.strictEqual(process.env.foo, undefined);
    }

    // Test process env ignore getter setter https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-env-ignore-getter-setter.js
    {
      assert.throws(
        () => {
          Object.defineProperty(process.env, 'foo', {
            value: 'foo1',
          });
        },
        {
          code: 'ERR_INVALID_ARG_VALUE',
          name: 'TypeError',
        }
      );

      assert.strictEqual(process.env.foo, undefined);
      process.env.foo = 'foo2';
      assert.strictEqual(process.env.foo, 'foo2');

      assert.throws(
        () => {
          Object.defineProperty(process.env, 'goo', {
            get() {
              return 'goo';
            },
            set() {},
          });
        },
        {
          code: 'ERR_INVALID_ARG_VALUE',
          name: 'TypeError',
        }
      );

      const attributes = ['configurable', 'writable', 'enumerable'];

      for (const attribute of attributes) {
        assert.throws(
          () => {
            Object.defineProperty(process.env, 'goo', {
              [attribute]: false,
            });
          },
          {
            code: 'ERR_INVALID_ARG_VALUE',
            name: 'TypeError',
          }
        );
      }

      assert.strictEqual(process.env.goo, undefined);
      Object.defineProperty(process.env, 'goo', {
        value: 'goo',
        configurable: true,
        writable: true,
        enumerable: true,
      });
      assert.strictEqual(process.env.goo, 'goo');
    }

    // Process env symbols (https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-env-symbols.js)
    {
      const symbol = Symbol('sym');

      // Verify that getting via a symbol key returns undefined.
      assert.strictEqual(process.env[symbol], undefined);

      // Verify that assigning via a symbol key throws.
      // The message depends on the JavaScript engine and so will be different between
      // different JavaScript engines. Confirm that the `Error` is a `TypeError` only.
      assert.throws(() => {
        process.env[symbol] = 42;
      }, TypeError);

      // Verify that assigning a symbol value throws.
      // The message depends on the JavaScript engine and so will be different between
      // different JavaScript engines. Confirm that the `Error` is a `TypeError` only.
      assert.throws(() => {
        process.env.foo = symbol;
      }, TypeError);

      // Verify that using a symbol with the in operator returns false.
      assert.strictEqual(symbol in process.env, false);

      // Verify that deleting a symbol key returns true.
      assert.strictEqual(delete process.env[symbol], true);

      // Checks that well-known symbols like `Symbol.toStringTag` won’t throw.
      Object.prototype.toString.call(process.env);
    }
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
    assert.throws(
      () => {
        process.setegid(1000);
      },
      { code: 'EPERM' }
    );
    assert.throws(
      () => {
        process.seteuid(1000);
      },
      { code: 'EPERM' }
    );
    process.setegid(0);
    assert.strictEqual(process.getegid(), 0);
    assert.strictEqual(process.geteuid(), 0);

    // Test other functions
    process.setSourceMapsEnabled(false); // Should be no-op

    // Test allowedNodeEnvironmentFlags
    assert.ok(process.allowedNodeEnvironmentFlags instanceof Set);
    assert.strictEqual(process.allowedNodeEnvironmentFlags.size, 0);
  },
};

// Properties which are undefined and NOT throwing stubs to ensure polyfillability
// via if (!process.prop) process.prop = polyfill();
// Some of these might be implemented at future dates.
export const processUndefined = {
  test() {
    // TODO(soon): Implement these!
    assert.strictEqual(process.stdin, undefined);
    assert.strictEqual(process.stdout, undefined);
    assert.strictEqual(process.stderr, undefined);

    // These may be implemented in future
    assert.strictEqual(process.kill, undefined);
    assert.strictEqual(process.ref, undefined);
    assert.strictEqual(process.unref, undefined);
    assert.strictEqual(process.exitCode, undefined);
    assert.strictEqual(process.channel, undefined);
    assert.strictEqual(process.connected, undefined);
    assert.strictEqual(process.binding, undefined);
    assert.strictEqual(process.debugPort, undefined);
    assert.strictEqual(process.dlopen, undefined);
    assert.strictEqual(process.finalization, undefined);
    assert.strictEqual(process.getActiveResourcesInfo, undefined);
    assert.strictEqual(process.setUncaughtExceptionCaptureCallback, undefined);
    assert.strictEqual(process.hasUncaughtExceptionCaptureCallback, undefined);
    assert.strictEqual(process.memoryUsage, undefined);
    assert.strictEqual(process.noDeprecation, undefined);
    assert.strictEqual(process.permission, undefined);
    assert.strictEqual(process.release, undefined);
    assert.strictEqual(process.report, undefined);
    assert.strictEqual(process.resourceUsage, undefined);
    assert.strictEqual(process.send, undefined);
    assert.strictEqual(process.traceDeprecation, undefined);
    assert.strictEqual(process.throwDeprecation, undefined);
    assert.strictEqual(process.sourceMapsEnabled, undefined);
    assert.strictEqual(process.threadCpuUsage, undefined);
  },
};

export const processVersions = {
  test() {
    assert.strictEqual(typeof process.versions, 'object');
    assert.ok(process.versions !== null);

    const expectedVersions = ['node'];

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
    assert.ok(process.features != null);

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
    await scheduler.wait(0);

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

export const processEmitWarning = {
  async test() {
    const testMsg = 'A Warning';
    const testCode = 'CODE001';
    const testDetail = 'Some detail';
    const testType = 'CustomWarning';

    let calledWarningHandler = false;
    process.on(
      'warning',
      (warning) => {
        calledWarningHandler = true;
        assert(warning);
        assert.match(warning.name, /^(?:Warning|CustomWarning)/);
        assert.strictEqual(warning.message, testMsg);
        if (warning.code) assert.strictEqual(warning.code, testCode);
        if (warning.detail) assert.strictEqual(warning.detail, testDetail);
      },
      15
    );

    class CustomWarning extends Error {
      constructor() {
        super();
        this.name = testType;
        this.message = testMsg;
        this.code = testCode;
        Error.captureStackTrace(this, CustomWarning);
      }
    }

    [
      [testMsg],
      [testMsg, testType],
      [testMsg, CustomWarning],
      [testMsg, testType, CustomWarning],
      [testMsg, testType, testCode],
      [testMsg, { type: testType }],
      [testMsg, { type: testType, code: testCode }],
      [testMsg, { type: testType, code: testCode, detail: testDetail }],
      [new CustomWarning()],
      // Detail will be ignored for the following. No errors thrown
      [testMsg, { type: testType, code: testCode, detail: true }],
      [testMsg, { type: testType, code: testCode, detail: [] }],
      [testMsg, { type: testType, code: testCode, detail: null }],
      [testMsg, { type: testType, code: testCode, detail: 1 }],
    ].forEach((args) => {
      process.emitWarning(...args);
    });

    const warningNoToString = new CustomWarning();
    warningNoToString.toString = null;
    process.emitWarning(warningNoToString);

    const warningThrowToString = new CustomWarning();
    warningThrowToString.toString = function () {
      throw new Error('invalid toString');
    };
    process.emitWarning(warningThrowToString);

    // TypeError is thrown on invalid input
    [
      [1],
      [{}],
      [true],
      [[]],
      ['', '', {}],
      ['', 1],
      ['', '', 1],
      ['', true],
      ['', '', true],
      ['', []],
      ['', '', []],
      [],
      [undefined, 'foo', 'bar'],
      [undefined],
    ].forEach((args) => {
      assert.throws(() => process.emitWarning(...args), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    });

    await new Promise(process.nextTick);
    assert(calledWarningHandler);
  },
};

export const processHrtime = {
  async test() {
    // process.hrtime
    // https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-hrtime.js
    {
      // The default behavior, return an Array "tuple" of numbers
      const tuple = process.hrtime();

      // Validate the default behavior
      validateTuple(tuple);

      // Validate that passing an existing tuple returns another valid tuple
      validateTuple(process.hrtime(tuple));

      // Test that only an Array may be passed to process.hrtime()
      assert.throws(
        () => {
          process.hrtime(1);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );
      assert.throws(
        () => {
          process.hrtime([]);
        },
        {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        }
      );
      assert.throws(
        () => {
          process.hrtime([1]);
        },
        {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        }
      );
      assert.throws(
        () => {
          process.hrtime([1, 2, 3]);
        },
        {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        }
      );

      function validateTuple(tuple) {
        assert(Array.isArray(tuple));
        assert.strictEqual(tuple.length, 2);
        assert(Number.isInteger(tuple[0]));
        assert(Number.isInteger(tuple[1]));
      }

      const diff = process.hrtime([0, 1e9 - 1]);
      assert(diff[1] >= 0); // https://github.com/nodejs/node/issues/4751
    }

    // process.hrtime.bigint
    // https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-hrtime-bigint.js
    {
      const start = process.hrtime.bigint();

      await scheduler.wait(1000);

      const end = process.hrtime.bigint();
      assert.strictEqual(typeof end, 'bigint');

      assert(end - start >= 0n);

      // Ideally, this should be closer to 1000, or we could test
      // a smaller interval, but this is to work around the
      // test runner time accuracy.
      assert(end - start >= 10_000_000n);
    }
  },
};

const basicValidEnv = `BASIC=overriden
`;

const validEnv = `BASIC=basic

# COMMENTS=work
#BASIC=basic2
#BASIC=basic3

# previous line intentionally left blank
AFTER_LINE=after_line
A="B=C"
B=C=D
EMPTY=
EMPTY_SINGLE_QUOTES=''
EMPTY_DOUBLE_QUOTES=""
EMPTY_BACKTICKS=\`\`
SINGLE_QUOTES='single_quotes'
SINGLE_QUOTES_SPACED='    single quotes    '
DOUBLE_QUOTES="double_quotes"
DOUBLE_QUOTES_SPACED="    double quotes    "
DOUBLE_QUOTES_INSIDE_SINGLE='double "quotes" work inside single quotes'
DOUBLE_QUOTES_WITH_NO_SPACE_BRACKET="{ port: $MONGOLAB_PORT}"
SINGLE_QUOTES_INSIDE_DOUBLE="single 'quotes' work inside double quotes"
BACKTICKS_INSIDE_SINGLE='\`backticks\` work inside single quotes'
BACKTICKS_INSIDE_DOUBLE="\`backticks\` work inside double quotes"
BACKTICKS=\`backticks\`
BACKTICKS_SPACED=\`    backticks    \`
DOUBLE_QUOTES_INSIDE_BACKTICKS=\`double "quotes" work inside backticks\`
SINGLE_QUOTES_INSIDE_BACKTICKS=\`single 'quotes' work inside backticks\`
DOUBLE_AND_SINGLE_QUOTES_INSIDE_BACKTICKS=\`double "quotes" and single 'quotes' work inside backticks\`
EXPAND_NEWLINES="expand\\nnew\\nlines"
DONT_EXPAND_UNQUOTED=dontexpand\\nnewlines
DONT_EXPAND_SQUOTED='dontexpand\\nnewlines'
# COMMENTS=work
INLINE_COMMENTS=inline comments # work #very #well
INLINE_COMMENTS_SINGLE_QUOTES='inline comments outside of #singlequotes' # work
INLINE_COMMENTS_DOUBLE_QUOTES="inline comments outside of #doublequotes" # work
INLINE_COMMENTS_BACKTICKS=\`inline comments outside of #backticks\` # work
INLINE_COMMENTS_SPACE=inline comments start with a#number sign. no space required.
EQUAL_SIGNS=equals==
RETAIN_INNER_QUOTES={"foo": "bar"}
RETAIN_INNER_QUOTES_AS_STRING='{"foo": "bar"}'
RETAIN_INNER_QUOTES_AS_BACKTICKS=\`{"foo": "bar's"}\`
TRIM_SPACE_FROM_UNQUOTED=    some spaced out string
SPACE_BEFORE_DOUBLE_QUOTES=   "space before double quotes"
EMAIL=therealnerdybeast@example.tld
    SPACED_KEY = parsed
EDGE_CASE_INLINE_COMMENTS="VALUE1" # or "VALUE2" or "VALUE3"

MULTI_DOUBLE_QUOTED="THIS
IS
A
MULTILINE
STRING"

MULTI_SINGLE_QUOTED='THIS
IS
A
MULTILINE
STRING'

MULTI_BACKTICKED=\`THIS
IS
A
"MULTILINE'S"
STRING\`
export EXPORT_EXAMPLE = ignore export

MULTI_NOT_VALID_QUOTE="
MULTI_NOT_VALID=THIS
IS NOT MULTILINE`;

// process.loadEnvFile
// https://github.com/nodejs/node/blob/5f7dbf45a3d3e3070d5f58f9a9c2c43dbecc8672/test/parallel/test-process-load-env-file.js
export const processLoadEnvFile = {
  async test() {
    const basicValidEnvFilePath = '/tmp/basic-valid.env';
    const validEnvFilePath = '/tmp/valid.env';
    const missingEnvFilePath =
      '/tmp/dir%20with unusual"chars \'åß∂ƒ©∆¬…`/non-existent-file.env';

    // Test prep: write the basic env file and valid env file
    writeFileSync(basicValidEnvFilePath, basicValidEnv);
    writeFileSync(validEnvFilePath, validEnv);

    // supports passing path
    {
      process.loadEnvFile(validEnvFilePath);
      assert.strictEqual(process.env.BASIC, 'basic');
    }

    // supports not-passing a path
    {
      // Uses `/bundle/.env` file.
      try {
        process.loadEnvFile();
        assert.fail();
      } catch (e) {
        assert.strictEqual(e.code, 'ENOENT');
        // TODO(soon): Enable once `path` is supported on ENOENT
        // assert.strictEqual(e.path, '/bundle/.env');
      }
    }

    // fails on missing paths
    {
      try {
        process.loadEnvFile(missingEnvFilePath);
      } catch (e) {
        assert.strictEqual(e.code, 'ENOENT');
        // TODO(soon): Enable once `path` is supported on ENOENT errors
        // assert.strictEqual(e.path, missingEnvFile);
      }
    }

    // should override
    {
      process.loadEnvFile(basicValidEnvFilePath);
      assert.strictEqual(process.env.BASIC, 'overriden');
    }

    // supports cwd
    {
      const originalCwd = process.cwd();
      try {
        process.chdir('/tmp');
        assert.throws(
          () => {
            process.loadEnvFile();
          },
          { code: 'ENOENT' }
        );

        writeFileSync('.env', validEnv);
        process.loadEnvFile();
      } finally {
        process.chdir(originalCwd);
      }
    }
  },
};

export const processRejectionListeners = {
  async test() {
    const e = new Error();

    const { promise: unhandledPromise, resolve } = Promise.withResolvers();
    process.on('unhandledRejection', (reason, promise) => {
      resolve({ reason, promise });
    });
    Promise.reject(e);
    const { reason, promise } = await unhandledPromise;
    assert.strictEqual(reason, e);

    {
      const { promise: handledPromise, resolve } = Promise.withResolvers();
      process.on('rejectionHandled', (promise) => {
        resolve({ promise });
      });
      await new Promise((resolve) => promise.catch(resolve));
      const { promise: promise2 } = await handledPromise;
      assert.strictEqual(promise, promise2);
    }
  },
};

export const processCwd = {
  test() {
    const cwd = process.cwd();
    assert.strictEqual(typeof cwd, 'string');
    assert.ok(cwd.length > 0);

    const originalCwd = process.cwd();

    process.chdir('/tmp');
    assert.strictEqual(process.cwd(), '/tmp');

    process.chdir(originalCwd);
    assert.strictEqual(process.cwd(), originalCwd);

    assert.throws(
      () => {
        process.chdir('/nonexistent/directory');
      },
      { code: 'ENOENT' }
    );
  },
};

export const processCwdRelative = {
  test() {
    const originalCwd = process.cwd();

    // Test relative path navigation
    process.chdir('/tmp');
    assert.strictEqual(process.cwd(), '/tmp');

    // Test going up from /tmp (should work if parent exists)
    try {
      process.chdir('..');
      const newCwd = process.cwd();
      assert.strictEqual(newCwd, '/');
    } catch (e) {
      // If parent doesn't exist, that's fine for this test
      if (e.code !== 'ENOENT') {
        throw e;
      }
    }

    // Test relative path from root
    process.chdir('/');
    process.chdir('tmp');
    assert.strictEqual(process.cwd(), '/tmp');

    // Test current directory "."
    process.chdir('.');
    assert.strictEqual(process.cwd(), '/tmp');

    // Restore original directory
    process.chdir(originalCwd);
    assert.strictEqual(process.cwd(), originalCwd);
  },
};

export const processCwdBadInput = {
  test() {
    const longPath = 'a'.repeat(4097); // Just over the path length limit
    assert.throws(
      () => {
        process.chdir(longPath);
      },
      { code: 'ENAMETOOLONG' }
    );

    try {
      process.chdir('/tmp/basic-valid.env');
      assert.fail('Expected chdir to throw an error');
    } catch (err) {
      assert.strictEqual(err.code, 'ENOENT');
    }

    assert.throws(
      () => {
        process.chdir('');
      },
      { code: 'ENOENT' }
    );
  },
};

export const processUmask = {
  test() {
    assert.strictEqual(typeof process.umask, 'function');

    assert.strictEqual(process.umask(), 18);

    assert.strictEqual(process.umask(0), 18);
    assert.strictEqual(process.umask(0o022), 18);
    assert.strictEqual(process.umask(0o755), 18);
    assert.strictEqual(process.umask(0xffffffff), 18);

    assert.strictEqual(process.umask('0'), 18);
    assert.strictEqual(process.umask('022'), 18);
    assert.strictEqual(process.umask('755'), 18);
    assert.strictEqual(process.umask('37777777777'), 18); // max 32-bit octal

    assert.throws(() => process.umask('8'), { code: 'ERR_INVALID_ARG_VALUE' });
    assert.throws(() => process.umask('9'), { code: 'ERR_INVALID_ARG_VALUE' });
    assert.throws(() => process.umask('abc'), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
    assert.throws(() => process.umask('0x123'), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
    assert.throws(() => process.umask(''), { code: 'ERR_INVALID_ARG_VALUE' });

    assert.throws(() => process.umask(-1), { code: 'ERR_INVALID_ARG_VALUE' });
    assert.throws(() => process.umask(1.5), { code: 'ERR_INVALID_ARG_VALUE' });
    assert.throws(
      () => process.umask(0x100000000), // 2^32
      { code: 'ERR_INVALID_ARG_VALUE' }
    );
    assert.throws(() => process.umask(NaN), { code: 'ERR_INVALID_ARG_VALUE' });
    assert.throws(() => process.umask(Infinity), {
      code: 'ERR_INVALID_ARG_VALUE',
    });

    assert.throws(() => process.umask({}), { code: 'ERR_INVALID_ARG_TYPE' });
    assert.throws(() => process.umask([]), { code: 'ERR_INVALID_ARG_TYPE' });
    assert.throws(() => process.umask(null), { code: 'ERR_INVALID_ARG_TYPE' });
    assert.throws(() => process.umask(true), { code: 'ERR_INVALID_ARG_TYPE' });

    assert.throws(
      () => process.umask('40000000000'), // > 32-bit max octal
      { code: 'ERR_INVALID_ARG_VALUE' }
    );
  },
};
