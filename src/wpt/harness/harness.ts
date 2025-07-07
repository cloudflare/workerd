// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright © web-platform-tests contributors. BSD license
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { default as path } from 'node:path';
import {
  FilterList,
  UnknownFunc,
  sanitize_unpaired_surrogates,
  getHostInfo,
  getBindingPath,
} from './common';

import { Test } from './test';

// These imports introduce functions into the global scope, so that WPT tests can call them

// Test, promise_test, async_test, test
import './test';

// WPT assert_ functions
import './assertions';

// Other functions from the WPT harness that need to be exposed to tests
import './utils';

// Modifications we need to make to global types for testing
import './globals';

type CommonOptions = {
  // Brief explanation of why these options were set for a test
  comment?: string;
  // Print information about each subtest within this test as it runs
  verbose?: boolean;
  // Function executed before running the test
  before?: () => void;
  // Function executed after running the test
  after?: () => void;
  // Function that modifies the test code before it's run
  replace?: (code: string) => string;
  // Execute only this test
  only?: boolean;
  // If true, evaluate the test code within a top-level block. Otherwise, the test runs within the
  // scope of an anonymous function.
  runInGlobalScope?: boolean;
};

type SuccessOptions = {
  expectedFailures?: undefined;
  disabledTests?: undefined;
  omittedTests?: undefined;
};

type ErrorOptions = {
  // A comment is mandatory when there are expected failure, skipped tests or excluded tests
  comment: string;

  // Known errors when running the tests
  expectedFailures?: (string | RegExp)[] | true;

  // Tests that cannot be run for now, either due to harness limitations, workerd bugs or
  // features not yet implemented.
  disabledTests?: (string | RegExp)[] | true;

  // Tests that we have decided not to use. These are omitted from results and coverage
  // calculations.
  omittedTests?: (string | RegExp)[] | true;
};

type TestRunnerOptions = CommonOptions & (SuccessOptions | ErrorOptions);

export type TestRunnerConfig = {
  [key: string]: TestRunnerOptions;
};

type Env = {
  unsafe: { eval: (code: string) => void };
  HTTP_PORT: string | null;
  HTTPS_PORT: string | null;
  [key: string]: unknown;
};

type TestCase = {
  test(_: unknown, env: Env): Promise<void> | void;
};

// Singleton object used to pass test state between the runner and the harness functions available
// to the evaled  WPT test code.
class RunnerState {
  // URL corresponding to the current test file, based on the WPT directory structure.
  testUrl: URL;

  // Filename of the current test. Used in error messages.
  testFileName: string;

  // The worker environment. Used to allow fetching resources in the test.
  env: Env;

  // Makes test options available from assertion functions
  options: TestRunnerOptions;

  // A test is pushed to this list as soon as it is discovered
  subtests: Test[] = [];

  // Callbacks to be run once the entire test is done.
  completionCallbacks: UnknownFunc[] = [];

  constructor(
    testUrl: URL,
    testFileName: string,
    env: Env,
    options: TestRunnerOptions
  ) {
    this.testUrl = testUrl;
    this.testFileName = testFileName;
    this.env = env;
    this.options = options;
  }

  async validate(): Promise<void> {
    // Exception handling is set up on every promise in the test function that created it.
    await Promise.all(this.subtests.map((t) => t.promise));

    for (const cleanFn of this.completionCallbacks) {
      cleanFn();
    }

    const expectedFailures = new FilterList(this.options.expectedFailures);
    const unexpectedFailures = [];

    for (const subtest of this.subtests) {
      const err = subtest.error;

      if (err instanceof Error) {
        if (!expectedFailures.delete(err.message)) {
          err.message = sanitize_unpaired_surrogates(err.message);
          console.warn(err);
          unexpectedFailures.push(err.message);
        } else if (this.options.verbose) {
          err.message = sanitize_unpaired_surrogates(err.message);
          console.warn('Expected failure: ', err);
        }
      }
    }

    if (unexpectedFailures.length > 0) {
      console.error(
        'The following tests unexpectedly failed:',
        JSON.stringify(
          unexpectedFailures.map((v) => v.toString()),
          null,
          2
        )
      );
    }

    const unexpectedSuccess = expectedFailures.getUnmatched();
    // TODO(soon): Once we have a list of successful assertions, we should throw an error if a
    // regex also matches successful tests. This can be done once we implement wpt.fyi report
    // generation.

    if (unexpectedSuccess.size > 0) {
      console.error(
        'The following tests were expected to fail but instead succeeded:',
        JSON.stringify(
          [...unexpectedSuccess].map((v) => v.toString()),
          null,
          2
        )
      );
    }

    if (unexpectedFailures.length > 0 || unexpectedSuccess.size > 0) {
      throw new Error(
        `${this.testFileName} failed. Please update the test config.`
      );
    }
  }
}

declare global {
  // Current RunnerState
  var state: RunnerState;
  // All RunnerStates (to get results later)
  var results: { [file: string]: RunnerState };
}

const COLORS = {
  FOREGROUND: 30,
  BACKGROUND: 40,
  BLACK: 0,
  RED: 1,
  GREEN: 2,
  YELLOW: 3,
  BLUE: 4,
  MAGENTA: 5,
  CYAN: 6,
  WHITE: 7,
};

function colorPrint(
  fn: (...args: unknown[]) => void,
  color: number
): (...args: unknown[]) => void {
  return (...args: unknown[]): void => {
    fn(`\x1b[${color}m`, ...args, '\x1b[0m');
  };
}

globalThis.console.info = colorPrint(
  console.info,
  COLORS.FOREGROUND + COLORS.BLUE
);
globalThis.console.warn = colorPrint(
  console.warn,
  COLORS.FOREGROUND + COLORS.YELLOW
);
globalThis.console.error = colorPrint(
  console.error,
  COLORS.FOREGROUND + COLORS.RED
);

class WPTMetadata {
  // Specifies the Javascript global scopes for the test. (Not currently supported)
  global: string[] = [];
  // Specifies additional JS scripts to execute.
  scripts: string[] = [];
  // Adjusts the timeout for the tests in this file. (Not currently supported)
  timeout?: string;
  // Sets a human-readable title for the entire test file. (Not currently supported.)
  title?: string;
  // Specifies a variant of this test, which can be used in subsetTestByKey (Not currently supported.)
  variants: URLSearchParams[] = [];
}

function parseWptMetadata(code: string): WPTMetadata {
  const meta = new WPTMetadata();

  for (const { groups } of code.matchAll(
    /\/\/ META: (?<key>\w+)=(?<value>.+)/g
  )) {
    if (!groups || !groups.key || !groups.value) {
      continue;
    }

    switch (groups.key) {
      case 'global':
        meta.global = groups.value.split(',');
        break;

      case 'script': {
        meta.scripts.push(groups.value);
        break;
      }

      case 'timeout':
        meta.timeout = groups.value;
        break;

      case 'title':
        meta.title = groups.value;
        break;

      case 'variant':
        meta.variants.push(new URLSearchParams(groups.value));
        break;
    }
  }

  return meta;
}

const EXCLUDED_PATHS = new Set([
  // Implemented in harness.ts
  '/common/subset-tests-by-key.js',
  '/common/subset-tests.js',
  '/resources/utils.js',
  '/common/utils.js',
  '/common/get-host-info.sub.js',
  '/common/gc.js',
  '/common/sab.js',
]);

function replaceInterpolation(code: string): string {
  const hostInfo = getHostInfo();

  return code
    .replace('{{host}}', hostInfo.REMOTE_HOST)
    .replace('{{ports[http][0]}}', hostInfo.HTTP_PORT)
    .replace('{{ports[http][1]}}', hostInfo.HTTP_PORT)
    .replace('{{ports[https][0]}}', hostInfo.HTTPS_PORT);
}

function getCodeAtPath(
  env: Env,
  base: string,
  rawPath: string,
  replace?: (code: string) => string
): string {
  const bindingPath = getBindingPath(base, rawPath);

  if (EXCLUDED_PATHS.has(bindingPath)) {
    return '';
  }

  if (typeof env[bindingPath] != 'string') {
    // We only have access to the files explicitly declared in the .wd-test, not the full WPT
    // checkout, so it's possible for tests to include things we can't load.
    throw new Error(
      `Test file ${bindingPath} not found. Update wpt_test.bzl to handle this case.`
    );
  }

  let code = env[bindingPath];
  if (replace) {
    code = replace(code);
  }

  return replaceInterpolation(code);
}

function evalAsBlock(
  env: Env,
  runInGlobalScope: boolean,
  files: string[]
): void {
  const code = files.join('\n');

  if (runInGlobalScope) {
    // In this mode, the scope of const, let and function declarations is limited to stop tests
    // from interfering with each other.
    env.unsafe.eval(`{ ${code} }`);
  } else {
    // Additionally, use an IIFE to limit the scope of var and manipulation of the global scope
    env.unsafe.eval(`(function () { ${code} })();`);
  }
}

type Runner = {
  run: (file: string) => TestCase | Record<string, never>;
  printResults: () => TestCase;
};

export function createRunner(
  config: TestRunnerConfig,
  moduleBase: string,
  allTestFiles: string[]
): Runner {
  const testsNotFound = new Set(Object.keys(config)).difference(
    new Set(allTestFiles)
  );

  if (testsNotFound.size > 0) {
    throw new Error(
      `Configuration was provided for the following tests that have not been found in the WPT repo: ${[...testsNotFound].join(', ')}`
    );
  }

  const onlyFlagUsed = Object.values(config).some((options) => options.only);
  globalThis.results = {};

  return {
    run(file: string): TestCase | Record<string, never> {
      if (onlyFlagUsed && !config[file]?.only) {
        // Return an empty object to avoid printing extra output from all the other disabled test cases.
        return {};
      }

      return {
        async test(_: unknown, env: Env): Promise<void> {
          return runTest(config[file], env, moduleBase, file);
        },
      };
    },
    printResults(): TestCase {
      return {
        test(_: unknown, env: Env): void {
          printResults(config, allTestFiles, moduleBase, env);
        },
      };
    },
  };
}

async function runTest(
  options: TestRunnerOptions | undefined,
  env: Env,
  moduleBase: string,
  file: string
): Promise<void> {
  if (!options) {
    throw new Error(
      `Missing test configuration for ${file}. Specify '${file}': {} for default options.`
    );
  }

  const testUrl = new URL(path.join(moduleBase, file), 'http://localhost');

  // If the environment variable HTTP_PORT is set, the wpt server is running as a sidecar.
  // Update the URL's port so we can connect to it
  testUrl.port = env.HTTP_PORT ?? '';

  globalThis.state = new RunnerState(testUrl, file, env, options);
  globalThis.results[file] = globalThis.state;

  if (options.disabledTests === true) {
    console.warn(
      `All tests in ${file} have been disabled because "${options.comment}".`
    );
    return;
  }

  if (options.omittedTests === true) {
    console.warn(
      `All tests in ${file} have been omitted because "${options.comment}".`
    );
    return;
  }

  const meta = parseWptMetadata(String(env[file]));

  if (options.before) {
    options.before();
  }

  const files = [];

  for (const script of meta.scripts) {
    files.push(getCodeAtPath(env, path.dirname(file), script));
  }

  files.push(getCodeAtPath(env, './', file, options.replace));
  evalAsBlock(env, options.runInGlobalScope ?? false, files);

  if (options.after) {
    options.after();
  }

  await globalThis.state.validate();
}

function printResults(
  config: TestRunnerConfig,
  allTestFiles: string[],
  moduleBase: string,
  env: Env
): void {
  const results: string[] = [];

  if (env.GEN_TEST_CONFIG) {
    results.push(generateConfig(config, allTestFiles, moduleBase));
  }

  if (env.GEN_TEST_REPORT) {
    results.push(generateReport(config));
  }

  if (env.GEN_TEST_STATS) {
    results.push(generateStats(moduleBase, config));
  }

  console.log(results.join('\n\n***\n\n'));
}

function generateConfig(
  config: TestRunnerConfig,
  allTestFiles: string[],
  moduleBase: string
): string {
  const generatedConfig: TestRunnerConfig = {};
  for (const file of allTestFiles) {
    // Copy config if the user has created any so far, else initialize to blank
    generatedConfig[file] = config[file] ?? {};
  }

  return `\x1b[1;7;35m*** Copy this config to src/wpt/${moduleBase}-test.ts ***\x1b[0m

// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default ${JSON.stringify(generatedConfig, null, 2)} satisfies TestRunnerConfig;`;
}

class WPTTestResult {
  test: string;
  status: 'OK' | 'ERROR';
  subtests: WPTSubtestResult[] = [];
  // TODO(soon): Track elapsed time
  duration: number = 0;

  constructor(result: RunnerState, options: TestRunnerOptions) {
    this.test = WPTTestResult.getTestNameFromUrl(result.testUrl);
    this.status = options.disabledTests === true ? 'ERROR' : 'OK';
    this.subtests = result.subtests.map((r) => new WPTSubtestResult(r));
  }

  private static getTestNameFromUrl(testUrl: URL): string {
    const testNameUrl = new URL(testUrl);
    testNameUrl.pathname = testNameUrl.pathname.replace('.js', '.html');
    return testNameUrl.href.slice(testNameUrl.origin.length);
  }
}

class WPTSubtestResult {
  name: string;
  status: 'PASS' | 'FAIL';
  message?: string;
  isExpectedFailure?: true;

  constructor(result: Test) {
    this.name = sanitize_unpaired_surrogates(result.name);
    if (result.error instanceof Error) {
      this.message = result.error.message;
      // TODO(soon): This is true in main, but not necessarily if you run a report in local dev
      this.isExpectedFailure = true;
      this.status = 'FAIL';
    } else if (result.error === 'DISABLED') {
      this.isExpectedFailure = true;
      this.status = 'FAIL';
    } else {
      this.status = 'PASS';
    }
  }
}

function generateReport(config: TestRunnerConfig): string {
  const results: WPTTestResult[] = [];
  for (const [file, options] of Object.entries(config)) {
    const testResult = globalThis.results[file];
    if (!testResult) {
      throw new Error(
        `Unable to find test results for ${file}. This is probably a harness bug`
      );
    }

    results.push(new WPTTestResult(testResult, options));
  }

  return JSON.stringify({ results }, null, 2);
}

class Stats {
  moduleBase: string;
  coverage = new CoverageStats();
  pass = new PassStats();

  constructor(moduleBase: string) {
    this.moduleBase = moduleBase;
  }

  toList(): unknown[] {
    return [this.moduleBase, ...this.coverage.toList(), ...this.pass.toList()];
  }
}
class CoverageStats {
  ok: number = 0;
  disabled: number = 0;

  get total(): number {
    return this.ok + this.disabled;
  }

  get ok_percent(): number {
    return (this.ok / this.total) * 100;
  }

  toList(): unknown[] {
    return [this.ok, this.disabled, this.total, this.ok_percent.toFixed(0)];
  }
}

class PassStats {
  pass: number = 0;
  fail: number = 0;
  disabled: number = 0;

  get total(): number {
    return this.pass + this.fail + this.disabled;
  }

  get pass_percent(): number {
    return (this.pass / this.total) * 100;
  }

  toList(): unknown[] {
    return [
      this.pass,
      this.fail,
      this.disabled,
      this.total,
      this.pass_percent.toFixed(0),
    ];
  }
}

function generateStats(moduleBase: string, config: TestRunnerConfig): string {
  const stats = new Stats(moduleBase);

  for (const [file, options] of Object.entries(config)) {
    if (options.disabledTests === true) {
      stats.coverage.disabled++;
    } else if (options.omittedTests !== true) {
      stats.coverage.ok++;
    }

    const testResult = globalThis.results[file];

    if (!testResult) {
      throw new Error(
        `Unable to find test results for ${file}. This is probably a harness bug`
      );
    }

    for (const subtestResult of testResult.subtests) {
      if (subtestResult.error === 'DISABLED') {
        stats.pass.disabled++;
      } else if (subtestResult.error instanceof Error) {
        stats.pass.fail++;
      } else if (subtestResult.error === undefined) {
        stats.pass.pass++;
      }
    }
  }

  return JSON.stringify(stats.toList());
}
