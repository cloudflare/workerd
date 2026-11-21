// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for the native (Rust) implementation of `node-internal:histogram`
// (src/rust/api/histogram.rs). Arguments are validated by perf_hooks_histogram.ts.

export class Histogram {
  constructor(
    lowest: number | bigint,
    highest: number | bigint,
    figures: number,
    halfLife: number,
    threshold: number
  );
  count(): number;
  countBigInt(): bigint;
  exceeds(): number;
  exceedsBigInt(): bigint;
  resetCount(): number;
  min(): number;
  minBigInt(): bigint;
  max(): number;
  maxBigInt(): bigint;
  mean(): number;
  stddev(): number;
  skewness(): number;
  kurtosis(): number;
  ewmaMean(): number;
  ewmaStddev(): number;
  ewmaErrorRate(): number;
  percentile(percentile: number): number;
  percentileBigInt(percentile: number): bigint;
  // Flat [percentile, value, ...] pairs.
  percentiles(): number[];
  // The percentiles and their values, in iteration order.
  percentilesBigInt(): { percentiles: number[]; values: bigint[] };
  record(value: number): void;
  recordBigInt(value: bigint): void;
  // `now` is the current time in milliseconds.
  recordDelta(now: number): void;
  recordCorrected(value: number, expectedInterval: number): void;
  recordCorrectedBigInt(value: bigint, expectedInterval: bigint): void;
  add(other: Histogram): void;
  subtract(other: Histogram): void;
  reset(): void;
  countAt(value: number): number;
  cdf(value: number): number;
  // `percentiles` must be ascending.
  percentilesAt(percentiles: number[]): number[];
  // Flat [value, count, ...] pairs.
  linearBuckets(stepSize: number): number[];
  logBuckets(firstBucket: number, base: number): number[];
  // [mean, lower, upper]
  meanCI(confidence: number): number[];
  // [value, lower, upper]
  percentileCI(percentile: number, confidence: number): number[];
  ksTest(other: Histogram): number;
  cohensD(other: Histogram): number;
  cliffsD(other: Histogram): number;
  // [tStatistic, degreesOfFreedom, pValue, ciLower, ciUpper]
  welchTest(other: Histogram, confidence: number): number[];
  // [uStatistic, zScore, pValue]
  mannWhitneyTest(other: Histogram): number[];
  // `mode` is the index of the dequantization mode in ['none', 'hdr', 'all'].
  qrde(
    probabilities: number[],
    mode: number
  ): {
    quantiles: number[];
    densities: number[];
    count: bigint;
    // The number of buckets with a nonzero count.
    buckets: number;
    corrections: number;
  };
  snapshot(): Histogram;
  // `error` is 0 on success, 1 for an incompatible layout, 2 for a reset since `other` was taken,
  // and 3 for an `other` that holds values this histogram does not.
  diff(other: Histogram): { histogram: Histogram | undefined; error: number };
  export(): Uint8Array;
}

declare const moduleDefault: {
  Histogram: typeof Histogram;
  // `undefined` if `data` is not valid export data.
  import(data: Uint8Array): Histogram | undefined;
};

export default moduleDefault;
