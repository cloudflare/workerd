// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Scenarios for perf_hooks histograms, shared by perf-hooks-histogram-nodejs-test.js and
// perf-hooks-histogram-golden.mjs, which runs them in Node.js to generate the expected results.
// They must stay deterministic: no clocks, and datasets from integer arithmetic only. Results under
// a key that starts with `approx` are compared with a tolerance.

import { createHistogram, importHistogram } from 'node:perf_hooks';
import { inspect } from 'node:util';

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
  one: [7],
  zero: [0],
  edges: [1, 2, 3, 2047, 2048, 2049, 1e6, 2 ** 40, Number.MAX_SAFE_INTEGER],
  constant: Array(100).fill(42),
  uniform: generate(1, 2000, (s) => 1 + (s % 10000)),
  // Values spread over many orders of magnitude.
  wide: generate(7, 1500, (s) => 1 + (s % 2 ** (1 + (s % 50)))),
  // Mostly small values with a long tail.
  tail: generate(11, 1000, (s) =>
    s % 100 === 0 ? 1 + (s % 1e9) : 1 + (s % 50)
  ),
};

const layouts = {
  default: undefined,
  small: { lowest: 1, highest: 11, figures: 1 },
  custom: { lowest: 10, highest: 100_000, figures: 2 },
  precise: { lowest: 1, highest: 10_000, figures: 5 },
  bigint: { lowest: 1000n, highest: 2n ** 62n, figures: 3 },
  ewma: { halfLife: 10, threshold: 5000 },
};

const kPercentiles = [0.0001, 1, 25, 50, 75, 90, 99, 99.9, 99.99, 100];

function dump(h) {
  return {
    count: h.count,
    countBigInt: String(h.countBigInt),
    min: h.min,
    minBigInt: String(h.minBigInt),
    max: h.max,
    maxBigInt: String(h.maxBigInt),
    mean: h.mean,
    stddev: h.stddev,
    skewness: h.skewness,
    kurtosis: h.kurtosis,
    exceeds: h.exceeds,
    exceedsBigInt: String(h.exceedsBigInt),
    ewmaMean: h.ewmaMean,
    ewmaStddev: h.ewmaStddev,
    ewmaErrorRate: h.ewmaErrorRate,
    resetCount: h.resetCount,
    percentiles: [...h.percentiles],
    percentilesBigInt: [...h.percentilesBigInt].map(([k, v]) => [k, String(v)]),
    percentile: h.count === 0 ? [] : kPercentiles.map((p) => h.percentile(p)),
    percentileBigInt:
      h.count === 0
        ? []
        : kPercentiles.map((p) => String(h.percentileBigInt(p))),
    toJSON: h.toJSON(),
    inspect: inspect(h),
  };
}

const kProbes = [0, 1, 7, 42, 100, 5000, 1e6, 2 ** 40, Number.MAX_SAFE_INTEGER];

// Whether `h` exports `data`, which holds every count and statistic.
function exports(h, data) {
  const actual = h.export();
  return (
    actual.length === data.length && actual.every((byte, i) => byte === data[i])
  );
}

// The results of h.qrde(options), as JSON.
async function qrde(h, options) {
  const r = await h.qrde(options);
  return {
    keys: Object.keys(r),
    objectProto: Object.getPrototypeOf(r) === Object.prototype,
    probabilities: [...r.probabilities],
    count: String(r.count),
    bucketCount: r.bucketCount,
    corrections: r.corrections,
    dequantize: r.dequantize,
    approxQuantiles: [...r.quantiles],
    approxDensities: [...r.densities],
  };
}

// The analysis methods, beyond what dump() covers.
async function analyze(h) {
  const linearStep = Math.max(1, Math.ceil(h.max / 50));
  const exported = h.export();
  return {
    cdf: kProbes.map((v) => h.cdf(v)),
    ccdf: kProbes.map((v) => h.ccdf(v)),
    countAt: kProbes.map((v) => h.countAt(v)),
    percentilesAt: [...h.percentilesAt([99, 50, 0.5, 100, 25, 99.999])],
    linearStep,
    linearBuckets: [...h.linearBuckets(linearStep)],
    logBuckets2: [...h.logBuckets(1, 2)],
    logBuckets10: [...h.logBuckets(10, 10)],
    percentileCI: [1, 50, 90, 99, 100].map((p) => ({ ...h.percentileCI(p) })),
    percentileCI80: { ...h.percentileCI(50, { confidence: 0.8 }) },
    burnRate: [0.5, 0.99, 0.999].map((t) => h.burnRate(t)),
    export: [...exported],
    imported: exports(importHistogram(exported), exported),
    snapshot: exports(h.snapshot(), exported),
    approxMeanCI: { ...h.meanCI() },
    approxMeanCI99: { ...h.meanCI({ confidence: 0.99 }) },
    approxQrde: await qrde(h, { bins: 16 }),
  };
}

// The comparisons of two histograms.
function compare(a, b) {
  const welch = a.welchTest(b);
  return {
    approxKsTest: a.ksTest(b),
    approxCohensD: a.cohensD(b),
    approxCliffsD: a.cliffsD(b),
    approxWelch: {
      ...welch,
      confidenceInterval: { ...welch.confidenceInterval },
    },
    approxWelch90: {
      ...a.welchTest(b, { confidence: 0.9 }).confidenceInterval,
    },
    approxMannWhitney: { ...a.mannWhitneyTest(b) },
  };
}

function error(f) {
  try {
    f();
  } catch (e) {
    return { name: e.name, code: e.code, message: e.message };
  }
  return null;
}

export async function run() {
  const results = {};

  results.empty = dump(createHistogram());
  results.emptyAnalysis = await analyze(createHistogram());

  for (const [layoutName, options] of Object.entries(layouts)) {
    for (const [dataName, values] of Object.entries(datasets)) {
      const h = createHistogram(options);
      for (const v of values) h.record(v);
      results[`${layoutName}/${dataName}`] = dump(h);
      if (layoutName !== 'bigint' && layoutName !== 'custom') {
        results[`${layoutName}/${dataName}/analysis`] = await analyze(h);
      }
    }
  }

  // qrde() with its other dequantization modes, and with its default 100 bins.
  for (const name of ['uniform', 'tail']) {
    const h = createHistogram();
    for (const v of datasets[name]) h.record(v);
    results[`qrde/${name}/none`] = await qrde(h, {
      bins: 5,
      dequantize: 'none',
    });
    results[`qrde/${name}/all`] = await qrde(h, {
      probabilities: [0, 0.1, 0.5, 0.9, 0.99, 1],
      dequantize: 'all',
    });
  }
  {
    const h = createHistogram();
    for (const v of datasets.tail) h.record(v);
    const r = await h.qrde();
    results.qrdeDefault = {
      ...r,
      probabilities: [...r.probabilities],
      count: String(r.count),
      approxQuantiles: [...r.quantiles],
      approxDensities: [...r.densities],
      quantiles: undefined,
      densities: undefined,
    };
  }

  // Comparisons between every pair of datasets.
  {
    const histograms = Object.entries(datasets).map(([name, values]) => {
      const h = createHistogram();
      for (const v of values) h.record(v);
      return [name, h];
    });
    histograms.push(['empty', createHistogram()]);
    for (const [aName, a] of histograms) {
      for (const [bName, b] of histograms) {
        results[`compare/${aName}/${bName}`] = compare(a, b);
      }
    }
  }

  // recordCorrected() backfills values a stall kept from being recorded.
  {
    const h = createHistogram();
    h.recordCorrected(10, 100);
    h.recordCorrected(1000, 100);
    h.recordCorrected(12345, 1000);
    h.recordCorrected(5n, 2n);
    h.recordCorrected(2n ** 40n, 2n ** 37n);
    results.recordCorrected = dump(h);
  }

  // subtract() clamps counts at zero and counts as a reset.
  {
    const a = createHistogram();
    const b = createHistogram();
    for (const v of datasets.uniform) a.record(v);
    for (const v of datasets.uniform.slice(0, 500)) b.record(v);
    b.record(1e9);
    a.subtract(b);
    results.subtractOther = dump(a);
    a.subtract(a);
    results.subtractSelf = dump(a);
    const c = createHistogram({ lowest: 10, highest: 100_000, figures: 2 });
    for (const v of datasets.uniform) c.record(v);
    const d = createHistogram();
    for (const v of datasets.uniform) d.record(v);
    d.subtract(c);
    results.subtractOtherLayout = dump(d);
  }

  // diff() of a histogram against an earlier snapshot of it.
  {
    const h = createHistogram({ lowest: 1, highest: 1e6, figures: 2 });
    for (const v of datasets.uniform.slice(0, 700)) h.record(v);
    const before = h.snapshot();
    for (const v of datasets.uniform.slice(700)) h.record(v);
    results.diff = dump(h.diff(before));
    results.diffSelf = dump(h.diff(h));
    results.diffShape = {
      snapshotName: before.constructor.name,
      snapshotRecord: typeof before.record,
      diffName: h.diff(before).constructor.name,
      importName: importHistogram(h.export()).constructor.name,
    };
    const later = h.snapshot();
    results.diffErrors = {
      notEarlier: error(() => before.diff(later)),
      incompatible: error(() => h.diff(createHistogram())),
      reset: error(() => {
        h.reset();
        h.diff(before);
      }),
      subtracted: error(() => {
        const a = createHistogram();
        a.record(5);
        const s = a.snapshot();
        a.subtract(a);
        a.diff(s);
      }),
    };
  }

  // Export data from Node.js imports, including the threshold and half-life.
  {
    const h = createHistogram({ halfLife: 10, threshold: 5000, figures: 2 });
    for (const v of datasets.tail) h.record(v);
    h.reset();
    for (const v of datasets.wide) h.record(v);
    const exported = h.export();
    results.exportEwma = [...exported];
    const imported = importHistogram(exported);
    results.importEwma = dump(imported);
    imported.record(3);
    results.importEwmaRecorded = dump(imported);
  }

  // Values out of the layout's range count as exceeds.
  {
    const h = createHistogram({ lowest: 10, highest: 1000, figures: 2 });
    for (const v of [1, 5, 10, 999, 1000, 1001, 5000, 1e9]) h.record(v);
    results.outOfRange = dump(h);
  }

  // BigInt records, including values beyond Number.MAX_SAFE_INTEGER.
  {
    const h = createHistogram({ highest: 2n ** 62n });
    for (const v of [
      1n,
      3n,
      2n ** 53n + 1n,
      2n ** 61n,
      2n ** 62n,
      2n ** 63n - 1n,
    ]) {
      h.record(v);
    }
    results.bigintRecords = dump(h);
  }

  // add(): another histogram, a histogram of another layout, and itself.
  {
    const a = createHistogram();
    const b = createHistogram();
    for (const v of datasets.uniform) a.record(v);
    for (const v of datasets.tail) b.record(v);
    b.record(2n ** 60n);
    a.add(b);
    results.addOther = dump(a);
    a.add(a);
    results.addSelf = dump(a);

    const c = createHistogram({ lowest: 10, highest: 100_000, figures: 2 });
    for (const v of datasets.wide) c.record(v);
    const d = createHistogram();
    d.add(c);
    results.addOtherLayout = dump(d);
  }

  // reset() clears values and exceeds, and counts resets.
  {
    const h = createHistogram({
      lowest: 1,
      highest: 100,
      figures: 2,
      halfLife: 3,
    });
    for (const v of [1, 50, 1000]) h.record(v);
    h.reset();
    results.afterReset = dump(h);
    for (const v of [2, 3]) h.record(v);
    h.reset();
    h.record(4);
    results.afterTwoResets = dump(h);
  }

  // A threshold that is not a safe integer.
  results.thresholdEdges = [Infinity, 1e300, NaN, 2.5].map((threshold) => {
    const h = createHistogram({ halfLife: 10, threshold });
    for (const v of datasets.tail) h.record(v);
    return dump(h);
  });

  // Argument validation.
  const h = createHistogram();
  const other = createHistogram();
  results.errors = {
    optionsString: error(() => createHistogram('hello')),
    optionsNull: error(() => createHistogram(null)),
    lowestZero: error(() => createHistogram({ lowest: 0 })),
    lowestFraction: error(() => createHistogram({ lowest: 1.5 })),
    lowestBigZero: error(() => createHistogram({ lowest: 0n })),
    lowestString: error(() => createHistogram({ lowest: '1' })),
    highestTooSmall: error(() => createHistogram({ lowest: 10, highest: 19 })),
    highestBigTooLarge: error(() => createHistogram({ highest: 2n ** 63n })),
    highestUnsafe: error(() => createHistogram({ highest: 2 ** 53 })),
    figuresZero: error(() => createHistogram({ figures: 0 })),
    figuresSix: error(() => createHistogram({ figures: 6 })),
    figuresBigInt: error(() => createHistogram({ figures: 3n })),
    halfLifeNegative: error(() => createHistogram({ halfLife: -1 })),
    halfLifeString: error(() => createHistogram({ halfLife: '1' })),
    thresholdNegative: error(() => createHistogram({ threshold: -1 })),
    recordNegative: error(() => h.record(-1)),
    recordFraction: error(() => h.record(1.5)),
    recordUnsafe: error(() => h.record(2 ** 53)),
    recordString: error(() => h.record('1')),
    recordUndefined: error(() => h.record()),
    recordBigNegative: error(() => h.record(-1n)),
    recordBigTooLarge: error(() => h.record(2n ** 63n)),
    percentileZero: error(() => h.percentile(0)),
    percentileAbove: error(() => h.percentile(100.5)),
    percentileNaN: error(() => h.percentile(NaN)),
    percentileString: error(() => h.percentile('50')),
    percentileBigIntZero: error(() => h.percentileBigInt(0)),
    addNumber: error(() => h.add(1)),
    addObject: error(() => h.add({})),
    illegalConstructor: error(() => new h.constructor()),
    illegalSuperConstructor: error(
      () => new (Object.getPrototypeOf(h.constructor))()
    ),
    invalidThisGetter: error(() =>
      Object.getOwnPropertyDescriptor(
        Object.getPrototypeOf(Object.getPrototypeOf(h)),
        'count'
      ).get.call({})
    ),
    invalidThisRecord: error(() => h.record.call({}, 1)),
    invalidThisAdd: error(() => h.add.call({}, other)),
    invalidThisCdf: error(() => h.cdf.call({}, 1)),
    invalidThisSnapshot: error(() => h.snapshot.call({})),
    invalidThisRecordCorrected: error(() =>
      h.recordCorrected.call(h.snapshot(), 1, 1)
    ),
    invalidThisSubtract: error(() => h.subtract.call(h.snapshot(), other)),
    cdfString: error(() => h.cdf('1')),
    ccdfUndefined: error(() => h.ccdf()),
    countAtBigInt: error(() => h.countAt(1n)),
    percentilesAtNotArray: error(() => h.percentilesAt(50)),
    percentilesAtZero: error(() => h.percentilesAt([50, 0])),
    percentilesAtString: error(() => h.percentilesAt(['50'])),
    linearBucketsZero: error(() => h.linearBuckets(0)),
    linearBucketsFraction: error(() => h.linearBuckets(1.5)),
    logBucketsZero: error(() => h.logBuckets(0, 2)),
    logBucketsBaseOne: error(() => h.logBuckets(1, 1)),
    logBucketsBaseString: error(() => h.logBuckets(1, '2')),
    meanCIOptions: error(() => h.meanCI(1)),
    meanCIConfidenceOne: error(() => h.meanCI({ confidence: 1 })),
    meanCIConfidenceNaN: error(() => h.meanCI({ confidence: NaN })),
    meanCIConfidenceString: error(() => h.meanCI({ confidence: '0.9' })),
    percentileCIZero: error(() => h.percentileCI(0)),
    percentileCIConfidence: error(() => h.percentileCI(50, { confidence: 0 })),
    burnRateOne: error(() => h.burnRate(1)),
    burnRateString: error(() => h.burnRate('0.9')),
    ksTestObject: error(() => h.ksTest({})),
    welchTestNumber: error(() => h.welchTest(1)),
    welchTestConfidence: error(() => h.welchTest(other, { confidence: 2 })),
    mannWhitneyUndefined: error(() => h.mannWhitneyTest()),
    cohensDObject: error(() => h.cohensD({})),
    cliffsDObject: error(() => h.cliffsD({})),
    diffObject: error(() => h.diff({})),
    subtractSnapshot: error(() => h.subtract(other.snapshot())),
    recordCorrectedNegative: error(() => h.recordCorrected(-1, 1)),
    recordCorrectedIntervalZero: error(() => h.recordCorrected(1, 0)),
    recordCorrectedMixed: error(() => h.recordCorrected(1n, 1)),
    recordCorrectedBigNegative: error(() => h.recordCorrected(-1n, 1n)),
    recordCorrectedBigIntervalZero: error(() => h.recordCorrected(1n, 0n)),
    recordCorrectedBigTooLarge: error(() => h.recordCorrected(2n ** 63n, 1n)),
    importString: error(() => importHistogram('hello')),
    importArrayBuffer: error(() => importHistogram(new ArrayBuffer(4))),
    importEmpty: error(() => importHistogram(new Uint8Array(0))),
    importGarbage: error(() => importHistogram(new Uint8Array([1, 2, 3]))),
    importTruncated: error(() =>
      importHistogram(createHistogram().export().subarray(0, 5))
    ),
  };

  // qrde() validation, which throws synchronously.
  results.qrdeErrors = {
    options: error(() => h.qrde(1)),
    exclusive: error(() => h.qrde({ bins: 2, probabilities: [0, 1] })),
    binsZero: error(() => h.qrde({ bins: 0 })),
    binsTooMany: error(() => h.qrde({ bins: 1001 })),
    binsFraction: error(() => h.qrde({ bins: 1.5 })),
    probabilitiesString: error(() => h.qrde({ probabilities: '01' })),
    // Node.js 26 words the message of a too-short array differently.
    probabilitiesShort: error(() => h.qrde({ probabilities: [0] })).code,
    probabilitiesLong: error(() =>
      h.qrde({
        probabilities: Array.from({ length: 1002 }, (_, i) => i / 1001),
      })
    ),
    probabilitiesRange: error(() => h.qrde({ probabilities: [0, 1.5] })),
    probabilitiesNaN: error(() => h.qrde({ probabilities: [0, NaN, 1] })),
    probabilitiesOrder: error(() =>
      h.qrde({ probabilities: [0, 0.5, 0.5, 1] })
    ),
    probabilitiesStart: error(() => h.qrde({ probabilities: [0.1, 1] })),
    probabilitiesEnd: error(() => h.qrde({ probabilities: [0, 0.9] })),
    dequantize: error(() => h.qrde({ dequantize: 'some' })),
    cache: error(() => h.qrde({ cache: 1 })),
  };

  results.shape = {
    constructorName: h.constructor.name,
    superName: Object.getPrototypeOf(h.constructor).name,
    inspectShallow: inspect(h, { depth: -1 }),
    jsonString: JSON.stringify(createHistogram()),
  };

  return results;
}
