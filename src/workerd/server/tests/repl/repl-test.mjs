// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { env, platform } from 'node:process';
import { test as nodeTest } from 'node:test';
import assert from 'node:assert';

assert(env.WORKERD_BINARY !== undefined, 'Set WORKERD_BINARY.');
assert(env.REPL_CONFIG !== undefined, 'Set REPL_CONFIG.');

// Each case starts workerd, which takes about two seconds under TSan, so Bazel runs the cases in
// shards (`shard_count`): a shard runs every SHARDS-th case declared, starting at its SHARD-th.
const SHARDS = Number(env.TEST_TOTAL_SHARDS ?? 1);
const SHARD = Number(env.TEST_SHARD_INDEX ?? 0);
if (env.TEST_SHARD_STATUS_FILE !== undefined) {
  await writeFile(env.TEST_SHARD_STATUS_FILE, '');
}
let declared = 0;
function test(...args) {
  if (declared++ % SHARDS === SHARD) {
    nodeTest(...args);
  }
}

// A build can make the compile caches for another binary's V8 flags
// (//build/config:compile_cache_v8_flags), and workerd then logs this warning as it starts. It
// isn't the REPL's output, so it's dropped from stderr.
const COMPILE_CACHE_WARNING =
  /^.*per-isolate bootstrap compile cache failed its compatibility check.*\n/gm;

// Runs `workerd repl --experimental ...args` with `input` as its stdin; its exit status, what it
// wrote to stdout, as text and as lines, and what it wrote to stderr.
function run(args, input) {
  return new Promise((resolve, reject) => {
    const child = spawn(
      env.WORKERD_BINARY,
      ['repl', '--experimental', ...args],
      {
        stdio: ['pipe', 'pipe', 'pipe'],
        env: { ...env, WORKERD_REPL_HISTORY: '' },
      }
    );
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (data) => (stdout += data));
    child.stderr.on('data', (data) => (stderr += data));
    child.on('error', reject);
    child.on('close', (code) => {
      // On Windows, what workerd writes through C's stdio, such as console.log() output, ends its
      // lines with \r\n, since stdio translates \n there.
      stdout = stdout.replaceAll('\r\n', '\n');
      stderr = stderr.replaceAll('\r\n', '\n');
      stderr = stderr.replace(COMPILE_CACHE_WARNING, '');
      resolve({
        code,
        stdout,
        results: stdout.split('\n').slice(0, -1),
        stderr,
      });
    });
    child.stdin.end(input);
  });
}

// Runs `workerd repl --experimental --interactive ...args`, which reads `lines` an input at a
// time; `results` are the lines of stdout, where the results go. A result can take several lines,
// as an object or an exception's stack does.
function repl(args, lines) {
  return run(
    ['--interactive', ...args],
    lines.map((line) => `${line}\n`).join('')
  );
}

test('a config named on the command line is loaded into `s`', async () => {
  const { code, stdout, results, stderr } = await repl(
    [env.REPL_CONFIG],
    ["await (await s.service('main').fetch('http://x/')).text()"]
  );
  assert.strictEqual(code, 0, stderr);
  assert(stdout.startsWith('ReplServer {'), stdout);
  assert.strictEqual(results.at(-1), "'hello'", stdout);
  // Without a terminal, the REPL shows no banner or prompts, so stderr holds only what went
  // wrong, and nothing did.
  assert.strictEqual(stderr, '');
});

test('the command line can name the config constant', async () => {
  const { code, stdout, results, stderr } = await repl(
    [env.REPL_CONFIG, 'config'],
    ['s.closed']
  );
  assert.strictEqual(code, 0, stderr);
  assert(stdout.startsWith('ReplServer {'), stdout);
  assert.strictEqual(results.at(-1), 'false', stdout);
});

test('a config that does not load leaves the REPL running', async () => {
  const { code, stdout, results, stderr } = await repl(
    ['no-such-config.capnp'],
    ['typeof s', '1 + 1']
  );
  // The load is an input that failed.
  assert.strictEqual(code, 1, stderr);
  assert(stdout.startsWith('Uncaught Error: no-such-config.capnp'), stdout);
  assert.deepStrictEqual(results.slice(-2), ["'undefined'", '2'], stdout);
});

test('an input that awaits declares its names as one that does not would', async () => {
  const { results, stdout } = await repl(
    [],
    [
      'const x = 1',
      'const x = await 2',
      'let y = 1',
      'let y = await 2',
      'class K {}',
      'const K = await 1',
      'const fetch = await 1',
      '[typeof globalThis.fetch, fetch]',
      'var v = await 3',
      'globalThis.v',
      // A declaration prints `undefined`, and a `const` declared so can be reassigned.
      'const z = await 4',
      'z = 5',
    ]
  );
  assert.deepStrictEqual(
    results,
    [
      'undefined',
      "Uncaught SyntaxError: Identifier 'x' has already been declared",
      'undefined',
      "Uncaught SyntaxError: Identifier 'y' has already been declared",
      'undefined',
      "Uncaught SyntaxError: Identifier 'K' has already been declared",
      'undefined',
      "[ 'function', 1 ]",
      'undefined',
      '3',
      'undefined',
      '5',
    ],
    stdout
  );
});

test('the exit status says whether an input failed', async () => {
  for (const [lines, failed] of [
    [['1 + 1', '_error'], false],
    [['null.x', '1 + 1'], true],
    [["await Promise.reject(new Error('no'))"], true],
    [['.no-such-command'], true],
    // A result that renders as an exception does is a result.
    [['class Uncaught {}; new Uncaught()'], false],
  ]) {
    const { code, stderr } = await repl([], lines);
    assert.strictEqual(code, failed ? 1 : 0, `${lines}: ${stderr}`);
  }
});

test('--v8-flag passes a flag to V8, with or without `=`', async () => {
  for (const args of [
    ['--v8-flag=--expose-gc'],
    ['--v8-flag', '--expose-gc'],
  ]) {
    const { code, results, stderr } = await repl(args, ['typeof gc']);
    assert.strictEqual(code, 0, stderr);
    assert.deepStrictEqual(results, ["'function'"]);
  }
});

test('--v8-flag refuses a flag V8 does not know', async () => {
  const { code, stderr } = await repl(['--v8-flag=--no-such-v8-flag'], []);
  assert.strictEqual(code, 1, stderr);
  assert(stderr.includes('unrecognized V8 flag'), stderr);
  assert(stderr.includes('--no-such-v8-flag'), stderr);
});

test('--autogate enables an autogate', async () => {
  const input = [
    "(await import('workerd:unsafe')).default.isTestAutogateEnabled()",
  ];
  for (const [args, enabled] of [
    [['--autogate=test-workerd'], 'true'],
    [[], 'false'],
  ]) {
    const { code, results, stderr } = await repl(
      ['--compat-flag=unsafe_module', ...args],
      input
    );
    assert.strictEqual(code, 0, stderr);
    assert.deepStrictEqual(results, [enabled]);
  }
});

test('--autogate refuses a name that is not an autogate', async () => {
  const { code, stderr } = await repl(['--autogate=no-such-gate'], []);
  assert.strictEqual(code, 1, stderr);
  assert(stderr.includes('No such autogate: no-such-gate.'), stderr);
  assert(stderr.includes('test-workerd'), stderr);
});

test('an input left unfinished when the input ends is reported', async () => {
  const { code, results, stderr } = await repl(
    [],
    ['1', 'f(', "  'never closed'"]
  );
  assert.strictEqual(code, 1, stderr);
  assert.deepStrictEqual(results, ['1']);
  assert(
    stderr.includes(
      'Error: The input ended inside the `(` opened on its line 1, so it did not run.'
    ),
    stderr
  );
});

test('--input-timeout stops awaiting an input that takes longer', async () => {
  const { code, results, stderr } = await repl(
    ['--input-timeout=0.1'],
    ['await new Promise(() => {})', '1 + 1']
  );
  assert.strictEqual(code, 1, stderr);
  assert.deepStrictEqual(results, ['2']);
  assert(
    stderr.includes(
      'Error: Timed out after 100ms: no longer awaiting the input, whose promise may still settle.'
    ),
    stderr
  );
});

test('an input that starts with `{` is an object literal if it can be', async () => {
  const { code, results, stderr } = await repl(
    [],
    [
      '{ a: 1, b: 2 }',
      '{ a: await Promise.resolve(1) }',
      '{ let x = 1; x }',
      '{ a: 1 };',
    ]
  );
  assert.strictEqual(code, 0, stderr);
  assert.deepStrictEqual(results, ['{ a: 1, b: 2 }', '{ a: 1 }', '1', '1']);
});

test('an input with a `/` after a closing bracket finishes where JavaScript ends it', async () => {
  const { results, stdout } = await repl(
    [],
    [
      'String({ valueOf: () => 4 } / 2)',
      'if (true) /[(]/.test("(")',
      'while (false) /[(]/; 7',
      '`${ { valueOf: () => 4 } / 2 }`',
      'String({ valueOf: () => 4 } / 2) / 4',
      'if (true) /{/.test("{")',
      'if (true) /\\(/.test("(")',
      '[function () {} / 2]',
      'f([)',
      "'next'",
    ]
  );
  assert.deepStrictEqual(
    results,
    [
      "'2'",
      'true',
      '7',
      "'2'",
      '0.5',
      'true',
      'true',
      '[ NaN ]',
      "Uncaught SyntaxError: Unexpected token ')'",
      "'next'",
    ],
    stdout
  );
});

test('.break abandons an unfinished input', async () => {
  const { code, results, stderr } = await repl(
    [],
    ['.break', 'f(', "  'abandoned'", '.break', '1 + 1']
  );
  assert.strictEqual(code, 0, stderr);
  assert.deepStrictEqual(results, ['2']);
});

test('a failure in the background fails the exit status', async () => {
  for (const lines of [
    // A timer callback that throws, once it has fired.
    [
      "setTimeout(() => { throw new Error('timer') }, 0), 0",
      'await new Promise((resolve) => setTimeout(resolve, 10))',
    ],
    ["void Promise.reject(new Error('rejected'))"],
    [
      "(await import('cloudflare:workers')).waitUntil(Promise.reject(new Error('task')))",
    ],
  ]) {
    const { code, stderr } = await repl([], lines);
    assert.strictEqual(code, 1, `${lines}: ${stderr}`);
  }
});

test('piped stdin is one script', async () => {
  // A line can continue the statement before it, and a function declared last is declared
  // throughout, though the script awaits.
  const { code, stdout, stderr } = await run(
    [],
    `const words = ['b', 'a']
  .sort();
console.log(words.join(' '));
console.log(await new Promise((resolve) => setTimeout(() => resolve('later'), 10)));
declaredBelow();
function declaredBelow() { console.log('declared below'); }
`
  );
  assert.strictEqual(code, 0, stderr);
  assert.strictEqual(stdout, 'a b\nlater\ndeclared below\n');
  assert.strictEqual(stderr, '');
});

test('-i reads piped stdin an input at a time, and prints the results', async () => {
  for (const [args, stdout] of [
    [['-i'], '2\n'],
    [['--interactive'], '2\n'],
    [[], ''],
  ]) {
    const result = await run(args, '1 + 1\n');
    assert.strictEqual(result.code, 0, result.stderr);
    assert.strictEqual(result.stdout, stdout, `${args}`);
  }
});

test('an exception the script does not catch ends it', async () => {
  const { code, stdout, stderr } = await run(
    [],
    "console.log('before');\nnull.x;\nconsole.log('after');\n"
  );
  assert.strictEqual(code, 1, stderr);
  assert.strictEqual(stdout, 'before\n');
  assert(
    stderr.startsWith('Uncaught TypeError: Cannot read properties of null'),
    stderr
  );
});

test('the exit status says whether the script failed', async () => {
  for (const [args, script, failed] of [
    [[], '', false],
    [[], "try { null.x } catch {}\n'done'", false],
    [[], "await Promise.reject(new Error('no'))", true],
    [[], "void Promise.reject(new Error('unhandled'))", true],
    [[], 'f(', true],
    [['--input-timeout=0.1'], 'await new Promise(() => {})', true],
  ]) {
    const { code, stderr } = await run(args, script);
    assert.strictEqual(code, failed ? 1 : 0, `${script}: ${stderr}`);
  }
});

test('a script runs once the command line config is in `s`', async () => {
  const { code, stdout, stderr } = await run(
    [env.REPL_CONFIG],
    "console.log(await (await s.service('main').fetch('http://x/')).text())"
  );
  assert.strictEqual(code, 0, stderr);
  assert.strictEqual(stdout, 'hello\n');
  assert.strictEqual(stderr, '');
});

test('a script does not run when the command line config fails to load', async () => {
  const { code, stdout, stderr } = await run(
    ['no-such-config.capnp'],
    "console.log('ran')"
  );
  assert.strictEqual(code, 1, stderr);
  assert.strictEqual(stdout, '');
  assert(stderr.startsWith('Uncaught Error: no-such-config.capnp'), stderr);
});

// Starts `workerd repl --experimental ...args` for a test to give input to as it goes. With
// `ignoringInterrupts`, the REPL starts with SIGINT ignored, as a shell without job control starts
// a background job.
function startRepl(args, { ignoringInterrupts = false } = {}) {
  const command = [env.WORKERD_BINARY, 'repl', '--experimental', ...args];
  const [program, ...programArgs] = ignoringInterrupts
    ? ['/bin/sh', '-c', `trap '' INT; exec "$0" "$@"`, ...command]
    : command;
  const child = spawn(program, programArgs, {
    stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...env, WORKERD_REPL_HISTORY: '' },
  });
  const output = { stdout: '', stderr: '' };
  let waiting = [];
  const check = () => {
    waiting = waiting.filter(
      ({ until, resolve }) => !(until(output) && (resolve(), true))
    );
  };
  child.stdout.on('data', (data) => {
    output.stdout += data;
    check();
  });
  child.stderr.on('data', (data) => {
    output.stderr += data;
    check();
  });
  const exited = new Promise((resolve, reject) => {
    child.on('error', reject);
    child.on('close', (code, signal) => resolve({ code, signal }));
  });
  return {
    output,
    // Resolves to the exit status and the signal that ended the REPL, if one did.
    exited,
    send(line) {
      child.stdin.write(`${line}\n`);
    },
    // Presses Ctrl+C, as the terminal would signal it.
    interrupt() {
      child.kill('SIGINT');
    },
    // Resolves once `until(output)` holds.
    waitFor(until) {
      return new Promise((resolve) => {
        waiting.push({ until, resolve });
        check();
      });
    },
    // Ends the input; the exit status.
    async end() {
      child.stdin.end();
      return (await exited).code;
    },
  };
}

// A config whose worker answers every fetch with `text`.
function configAnswering(text) {
  return `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    ( name = "main",
      worker = (
        compatibilityDate = "2025-01-01",
        modules = [
          ( name = "worker.js",
            esModule = "export default { fetch() { return new Response('${text}'); } };" ),
        ],
      ),
    ),
  ],
);
`;
}

test('a watched config reloads when its file changes', async () => {
  const dir = await mkdtemp(join(env.TEST_TMPDIR ?? tmpdir(), 'repl-watch-'));
  const config = join(dir, 'config.capnp');
  await writeFile(config, configAnswering('one'));
  const fetchMain = "await (await s.service('main').fetch('http://x/')).text()";

  const repl = startRepl(['-i']);
  repl.send(
    `s = await workerd.loadConfig(${JSON.stringify(config)}, { watch: true })`
  );
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.includes("'one'"));

  await writeFile(config, configAnswering('two'));
  await repl.waitFor(({ stderr }) => stderr.includes('Reloaded '));
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.includes("'two'"));

  // A change that breaks the config leaves the server running.
  await writeFile(config, 'not a config');
  await repl.waitFor(({ stderr }) => stderr.includes('did not reload'));
  repl.send(fetchMain);
  await repl.waitFor(({ stdout }) => stdout.split("'two'").length === 3);

  assert.strictEqual(await repl.end(), 0, repl.output.stderr);
});

const unixOnly = {
  skip: platform === 'win32' && 'Ctrl+C abandons an input only on Unix',
};

test('Ctrl+C stops awaiting an input', unixOnly, async () => {
  const repl = startRepl(['-i']);
  repl.send("console.log('awaiting'); await new Promise(() => {})");
  await repl.waitFor(({ stdout }) => stdout.includes('awaiting'));
  repl.interrupt();
  await repl.waitFor(({ stderr }) => stderr.includes('Interrupted: '));
  repl.send('1 + 1');
  await repl.waitFor(({ stdout }) => stdout.endsWith('2\n'));
  assert.strictEqual(await repl.end(), 0, repl.output.stderr);
});

test(
  'a second Ctrl+C ends the REPL while an input runs without awaiting',
  unixOnly,
  async () => {
    const repl = startRepl(['-i']);
    repl.send("console.log('spinning'); while (true) {}");
    await repl.waitFor(({ stdout }) => stdout.includes('spinning'));
    // Signals that arrive together are taken as one, so this signals until the REPL ends.
    const interrupting = setInterval(() => repl.interrupt(), 10);
    const exit = await repl.exited;
    clearInterval(interrupting);
    assert.deepStrictEqual(exit, { code: null, signal: 'SIGINT' });
  }
);

test('a REPL started with SIGINT ignored ignores it', unixOnly, async () => {
  const repl = startRepl(['-i'], { ignoringInterrupts: true });
  repl.send("console.log('ready')");
  await repl.waitFor(({ stdout }) => stdout.includes('ready'));
  repl.interrupt();
  repl.send('1 + 1');
  // A REPL that SIGINT ends never prints the result, so wait on its exit too.
  await Promise.race([
    repl.waitFor(({ stdout }) => stdout.endsWith('2\n')),
    repl.exited.then((exit) =>
      assert.fail(`exited: ${JSON.stringify(exit)}\n${repl.output.stderr}`)
    ),
  ]);
  assert.strictEqual(await repl.end(), 0, repl.output.stderr);
});

test('Ctrl+C ends the REPL while a script runs', unixOnly, async () => {
  const repl = startRepl([]);
  repl.send("console.log('started'); await new Promise(() => {})");
  // The script runs once its input ends.
  const ended = repl.end();
  await repl.waitFor(({ stdout }) => stdout.includes('started'));
  repl.interrupt();
  assert.deepStrictEqual(await repl.exited, { code: null, signal: 'SIGINT' });
  await ended;
});

test('an environment evaluates input as the console does', async () => {
  const { code, results, stderr } = await repl(
    ['--compat-date=2025-01-01'],
    [
      "e = await workerd.environment({ compatibilityDate: '2022-01-01' })",
      "await e.eval('typeof navigator')",
      // A declaration in an input that awaits outlives the input there too.
      "await e.eval('const y = await Promise.resolve(1)')",
      "await e.eval('y + 1')",
      'typeof y',
    ]
  );
  assert.strictEqual(code, 0, stderr);
  assert.deepStrictEqual(
    results.slice(-4),
    [`"'undefined'"`, "'undefined'", "'2'", "'undefined'"],
    results.join('\n')
  );
});
