// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// Stands in for benchmark/common.js from Node.js when its benchmarks run under
// workerd. It supports the parts the buffer benchmarks use: createBenchmark,
// bench.start(), and bench.end(). The configuration queue is built as Node.js
// builds it, so the same index names the same configuration.
//
// Node.js forks a process per configuration; run.py does the same, starting
// workerd once per configuration. Node.js times with process.hrtime; here
// Date.now() is used, which advances during execution only with the
// precise_timers compatibility flag, at 3ms granularity, so run.py raises `n`
// until runs are long enough to measure.
//
// workerd does not allow new Function() while handling a request, so run.py
// rewrites the benchmarks' uses of it to call newFunction() here, which uses
// the unsafeEval binding.

'use strict';

let benchmark;
let unsafeEval;

class Benchmark {
  constructor(fn, configs, options = {}) {
    this.fn = fn;
    this.configs = configs;
    this.options = options;
    this.combinationFilter =
      typeof options.combinationFilter === 'function'
        ? options.combinationFilter
        : () => true;
    this._started = false;
    this._ended = false;
    this._time = 0;
  }

  // Node.js's _parseArgs, with `overrides` ({ key: [string values] }) in place
  // of command-line arguments.
  _parseArgs(configs, overrides) {
    const cliOptions = {};
    for (const [key, value] of Object.entries(configs)) {
      if (!Array.isArray(value)) configs[key] = [value];
    }
    for (const [key, values] of Object.entries(overrides)) {
      if (configs[key] === undefined) continue;
      const configType = typeof configs[key][0];
      cliOptions[key] = values.map((value) =>
        configType === 'number'
          ? +value
          : configType === 'boolean'
            ? value === 'true'
            : value
      );
    }
    return { ...configs, ...cliOptions };
  }

  // Node.js's _queue.
  _queue(options) {
    const queue = [];
    const keys = Object.keys(options);
    const { combinationFilter } = this;

    function recursive(keyIndex, prevConfig) {
      const key = keys[keyIndex];
      const values = options[key];

      for (const value of values) {
        if (
          typeof value !== 'number' &&
          typeof value !== 'string' &&
          typeof value !== 'boolean'
        ) {
          throw new TypeError(`configuration "${key}" had type ${typeof value}`);
        }
        if (typeof value !== typeof values[0]) {
          throw new TypeError(`configuration "${key}" has mixed types`);
        }

        const currConfig = { [key]: value, ...prevConfig };

        if (keyIndex + 1 < keys.length) {
          recursive(keyIndex + 1, currConfig);
        } else {
          const allowed = combinationFilter({ ...currConfig });
          if (typeof allowed !== 'boolean') {
            throw new TypeError('Combination filter must always return a boolean');
          }
          if (allowed) queue.push(currConfig);
        }
      }
    }

    if (keys.length > 0) {
      recursive(0, {});
    } else {
      queue.push({});
    }
    return queue;
  }

  queue(overrides) {
    const { configs, options } = this;
    if (!options.byGroups) {
      return this._queue(this._parseArgs(configs, overrides));
    }
    let queue = [];
    for (const groupName of Object.keys(configs)) {
      const config = { ...configs[groupName][0], group: groupName };
      queue = queue.concat(this._queue(this._parseArgs(config, overrides)));
    }
    return queue;
  }

  start() {
    if (this._started) {
      throw new Error('Called start more than once in a single benchmark');
    }
    this._started = true;
    this._time = Date.now();
  }

  end(operations) {
    const time = Date.now();
    if (!this._started) throw new Error('called end without start');
    if (this._ended) throw new Error('called end multiple times');
    if (typeof operations !== 'number') {
      throw new Error('called end() without specifying operation count');
    }
    if (operations <= 0) {
      throw new Error('called end() with operation count <= 0');
    }
    this._ended = true;
    if (time === this._time) {
      throw new Error('insufficient clock precision for short benchmark');
    }
    const elapsedMs = time - this._time;
    this._resolve({ rate: operations / (elapsedMs / 1000), elapsedMs });
  }
}

// new Function(...argNames, body).
exports.newFunction = function (...args) {
  const body = args.pop();
  return unsafeEval.newFunction(body, 'anonymous', ...args);
};

exports.createBenchmark = function (fn, configs, options) {
  benchmark = new Benchmark(fn, configs, options);
  return benchmark;
};

// Called by run.py's entry module. `mode` is 'list', which returns the
// configuration queue, or 'run', which runs configuration `index`, with `n`
// in place of its own when `n` is not null.
exports.run = async function (mode, index, overrides, n, env) {
  unsafeEval = env.UNSAFE_EVAL;
  const queue = benchmark.queue(overrides);
  if (mode === 'list') return { queue };
  const config = n === null ? queue[index] : { ...queue[index], n };
  const done = new Promise((resolve) => {
    benchmark._resolve = resolve;
  });
  await benchmark.fn(config);
  return { config, ...(await done) };
};
