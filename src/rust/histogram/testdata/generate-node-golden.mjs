// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Generates node-golden.json, the expected results of Node.js's perf_hooks histograms that the
// histogram crate's parity tests (lib-test.rs) compare against:
//
//   node src/rust/histogram/testdata/generate-node-golden.mjs \
//     > src/rust/histogram/testdata/node-golden.json
//
// Run it with Node.js 26.10 or later. The datasets are generated with integer arithmetic only, so
// they are the same on every platform, and they are stored in the output so that the tests do not
// need to reproduce them.

import { createHistogram } from 'node:perf_hooks';

const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER;

// MINSTD: exact in doubles, since the product stays below 2^53.
function minstd(seed) {
  let state = seed;
  return () => {
    state = (state * 48271) % 2147483647;
    return state;
  };
}

function generate(seed, n, f) {
  const next = minstd(seed);
  return Array.from({ length: n }, () => f(next()));
}

const datasets = {
  empty: [],
  one: [7],
  two: [3, 9],
  three: [1, 5, 1000],
  four: [2, 2, 8, 64],
  constant: Array(100).fill(42),
  uniform: generate(1, 2000, (s) => 1 + (s % 10000)),
  wide: generate(2, 5000, (s) => 1 + Math.floor(s / 2 ** (s % 31))),
  bimodal: generate(3, 3000, (s) =>
    s % 2 === 0 ? 100 + (s % 50) : 5000 + (s % 500)
  ),
  heavy: generate(4, 3000, (s) => Math.floor(1_000_000 / (1 + (s % 1000)))),
  huge: generate(5, 1000, (s) => s * 4096 + (s % 4096)),
};

const layouts = [
  { lowest: 1, highest: MAX_SAFE_INTEGER, figures: 3 },
  { lowest: 1, highest: 1e9, figures: 1 },
  { lowest: 1, highest: 1e9, figures: 2 },
  { lowest: 1, highest: 1e12, figures: 4 },
  { lowest: 1, highest: 1e9, figures: 5 },
  { lowest: 1000, highest: 1e12, figures: 3 },
  { lowest: 1024, highest: 1e12, figures: 2 },
];

const pairs = [
  ['uniform', 'wide'],
  ['bimodal', 'heavy'],
  ['uniform', 'constant'],
  ['three', 'four'],
  ['empty', 'uniform'],
  ['one', 'two'],
];

const percentiles = [0.1, 1, 12, 25, 50, 75, 90, 99, 99.9, 100];

// Floats are written as strings: JSON has no NaN or infinities, and serde_json's default float
// parsing is not correctly rounded, while Rust's str::parse::<f64>() is.
function num(x) {
  return String(x);
}

function entries(map) {
  return [...map].map(([k, v]) => [num(k), num(v)]);
}

function build(layout, values) {
  const h = createHistogram(layout);
  for (const v of values) h.record(v);
  return h;
}

async function describe(h) {
  const max = h.count > 0 ? h.max : 1;
  const step = Math.max(1, Math.floor(max / 20));
  const probes = [0, 1, 2, 7, 42, 100, 999, 1000, 5000, 1e6, max, max + 1];
  const result = {
    count: h.count,
    exceeds: h.exceeds,
    min: String(h.minBigInt),
    max: String(h.maxBigInt),
    mean: num(h.mean),
    stddev: num(h.stddev),
    percentile: percentiles.map((p) => String(h.percentileBigInt(p))),
    percentilesAt: entries(h.percentilesAt(percentiles)),
    percentiles: entries(h.percentiles),
    cdf: probes.map((v) => [v, num(h.cdf(v))]),
    countAt: probes.map((v) => [v, h.countAt(v)]),
    linearStep: step,
    linearBuckets: entries(h.linearBuckets(step)),
    logBuckets: entries(h.logBuckets(1, 2)),
    logBuckets10: entries(h.logBuckets(10, 10)),
    skewness: num(h.skewness),
    kurtosis: num(h.kurtosis),
    meanCI95: Object.values(h.meanCI({ confidence: 0.95 })).map(num),
    meanCI99: Object.values(h.meanCI({ confidence: 0.99 })).map(num),
    percentileCI: [
      [50, 0.95],
      [99, 0.95],
      [90, 0.99],
    ].map(([p, confidence]) => {
      const ci = h.percentileCI(p, { confidence });
      return [num(p), num(confidence), ci.value, ci.lower, ci.upper];
    }),
    export: Buffer.from(h.export()).toString('hex'),
  };
  if (h.count > 0) {
    result.qrde = [];
    for (const options of [
      { bins: 10, dequantize: 'hdr' },
      { bins: 7, dequantize: 'none' },
      { probabilities: [0, 0.05, 0.5, 0.95, 0.999, 1], dequantize: 'all' },
    ]) {
      const r = await h.qrde(options);
      result.qrde.push({
        probabilities: [...r.probabilities].map(num),
        dequantize: r.dequantize,
        quantiles: [...r.quantiles].map(num),
        densities: [...r.densities].map(num),
        corrections: r.corrections,
        bucketCount: r.bucketCount,
      });
    }
  }
  return result;
}

const out = { node: process.version, datasets, layouts: [] };
for (const layout of layouts) {
  const histograms = {};
  for (const [name, values] of Object.entries(datasets)) {
    histograms[name] = await describe(build(layout, values));
  }
  const comparisons = pairs.map(([a, b]) => {
    const ha = build(layout, datasets[a]);
    const hb = build(layout, datasets[b]);
    const welch = ha.welchTest(hb, { confidence: 0.95 });
    const mw = ha.mannWhitneyTest(hb);
    return {
      a,
      b,
      welch: [
        welch.tStatistic,
        welch.degreesOfFreedom,
        welch.pValue,
        welch.confidenceInterval.lower,
        welch.confidenceInterval.upper,
      ].map(num),
      mannWhitney: [mw.uStatistic, mw.zScore, mw.pValue].map(num),
      cohensD: num(ha.cohensD(hb)),
      cliffsD: num(ha.cliffsD(hb)),
      ksTest: num(ha.ksTest(hb)),
    };
  });
  out.layouts.push({ options: layout, histograms, comparisons });
}
process.stdout.write(JSON.stringify(out) + '\n');
