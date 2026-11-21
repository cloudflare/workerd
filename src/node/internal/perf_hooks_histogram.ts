// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

// The `Histogram` and `RecordableHistogram` classes of `node:perf_hooks`, ported from Node.js's
// lib/internal/histogram.js over the native histogram of `node-internal:histogram`, which matches
// Node.js's results exactly.

import { default as histogramImpl } from 'node-internal:histogram';
import { default as utilImpl } from 'node-internal:util';
import {
  ERR_ILLEGAL_CONSTRUCTOR,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_THIS,
  ERR_OUT_OF_RANGE,
  NodeError,
  NodeRangeError,
  NodeTypeError,
} from 'node-internal:internal_errors';
import {
  validateArray,
  validateBoolean,
  validateInteger,
  validateNumber,
  validateObject,
  validateOneOf,
} from 'node-internal:validators';
import { isUint8Array } from 'node-internal:internal_types';
import { kNullProtoObject } from 'node-internal:internal_utils';
import {
  customInspectSymbol,
  inspect,
  type InspectOptionsStylized,
} from 'node-internal:internal_inspect';

type Handle = InstanceType<typeof histogramImpl.Histogram>;

const kSkipThrow = Symbol('kSkipThrow');

// The `error` of the native `diff()`.
const kDiffIncompatible = 1;
const kDiffReset = 2;
const kDiffNotEarlier = 3;
const kMaxInt64 = 9_223_372_036_854_775_807n;

let handleOf: (value: unknown) => Handle | undefined;
let isRecordable: (value: unknown) => boolean;

function histogramHandle(value: unknown): Handle {
  const handle = handleOf(value);
  if (handle === undefined) throw new ERR_INVALID_THIS('Histogram');
  return handle;
}

function recordableHandle(value: unknown): Handle {
  if (!isRecordable(value)) throw new ERR_INVALID_THIS('RecordableHistogram');
  return histogramHandle(value);
}

function validatePercentile(
  percentile: unknown,
  name = 'percentile'
): asserts percentile is number {
  validateNumber(percentile, name);
  if (Number.isNaN(percentile) || percentile <= 0 || percentile > 100) {
    throw new ERR_OUT_OF_RANGE(name, '> 0 && <= 100', percentile);
  }
}

// Validates `options.confidence`, which defaults to 0.95.
function confidenceOption(options: unknown): number {
  validateObject(options, 'options');
  const { confidence = 0.95 } = options as { confidence?: unknown };
  validateNumber(confidence, 'options.confidence');
  if (Number.isNaN(confidence) || confidence <= 0 || confidence >= 1) {
    throw new ERR_OUT_OF_RANGE('options.confidence', '> 0 && < 1', confidence);
  }
  return confidence;
}

function otherHandle(other: unknown): Handle {
  const handle = handleOf(other);
  if (handle === undefined) {
    throw new ERR_INVALID_ARG_TYPE('other', 'Histogram', other);
  }
  return handle;
}

function bucketMap(pairs: number[]): Map<number, number> {
  const map = new Map<number, number>();
  for (let i = 0; i < pairs.length; i += 2) {
    map.set(pairs[i] as number, pairs[i + 1] as number);
  }
  return map;
}

const kQrdeDequantizationModes = ['none', 'hdr', 'all'];

export interface QrdeOptions {
  bins?: number;
  probabilities?: number[];
  dequantize?: 'none' | 'hdr' | 'all';
  cache?: boolean;
}

export interface QrdeResult {
  probabilities: Float64Array;
  quantiles: Float64Array;
  densities: Float64Array;
  count: bigint;
  bucketCount: number;
  corrections: number;
  dequantize: 'none' | 'hdr' | 'all';
}

function qrdeProbabilities(
  bins: unknown,
  probabilities: unknown,
  options: unknown
): number[] {
  if (bins !== undefined && probabilities !== undefined) {
    throw new ERR_INVALID_ARG_VALUE(
      'options',
      options,
      '"bins" and "probabilities" are mutually exclusive'
    );
  }
  if (probabilities === undefined) {
    const binCount = bins ?? 100;
    validateInteger(binCount, 'options.bins', 1, 1000);
    return Array.from({ length: binCount + 1 }, (_, i) => i / binCount);
  }
  validateArray(probabilities, 'options.probabilities', 2);
  const length = probabilities.length;
  if (length > 1001) {
    throw new ERR_OUT_OF_RANGE(
      'options.probabilities.length',
      '>= 2 && <= 1001',
      length
    );
  }
  let previous = -1;
  for (let i = 0; i < length; i++) {
    const probability = probabilities[i];
    validateNumber(probability, `options.probabilities[${i}]`, 0, 1);
    if (probability <= previous) {
      throw new ERR_INVALID_ARG_VALUE(
        'options.probabilities',
        probabilities,
        'must be strictly increasing'
      );
    }
    previous = probability;
  }
  if (probabilities[0] !== 0 || probabilities[length - 1] !== 1) {
    throw new ERR_INVALID_ARG_VALUE(
      'options.probabilities',
      probabilities,
      'must start with 0 and end with 1'
    );
  }
  return probabilities as number[];
}

export class Histogram {
  #handle: Handle;

  static {
    handleOf = (value: unknown): Handle | undefined =>
      typeof value === 'object' && value !== null && #handle in value
        ? value.#handle
        : undefined;
  }

  constructor(skipThrowSymbol: symbol, handle: Handle) {
    if (skipThrowSymbol !== kSkipThrow) {
      throw new ERR_ILLEGAL_CONSTRUCTOR();
    }
    this.#handle = handle;
    // Node.js clones a histogram, sharing its native handle, which workerd's serializer has no
    // way to do; a clone would otherwise be an empty plain object.
    utilImpl.markUncloneable(this);
  }

  [customInspectSymbol](
    depth: number,
    options: InspectOptionsStylized
  ): string {
    if (depth < 0) {
      return options.stylize(`[${this.constructor.name}]`, 'special');
    }

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `Histogram ${inspect(
      {
        min: this.min,
        max: this.max,
        mean: this.mean,
        exceeds: this.exceeds,
        stddev: this.stddev,
        skewness: this.skewness,
        kurtosis: this.kurtosis,
        count: this.count,
        percentiles: this.percentiles,
      },
      opts
    )}`;
  }

  get count(): number {
    return histogramHandle(this).count();
  }

  get countBigInt(): bigint {
    return histogramHandle(this).countBigInt();
  }

  get min(): number {
    return histogramHandle(this).min();
  }

  get minBigInt(): bigint {
    return histogramHandle(this).minBigInt();
  }

  get max(): number {
    return histogramHandle(this).max();
  }

  get maxBigInt(): bigint {
    return histogramHandle(this).maxBigInt();
  }

  get mean(): number {
    return histogramHandle(this).mean();
  }

  get exceeds(): number {
    return histogramHandle(this).exceeds();
  }

  get exceedsBigInt(): bigint {
    return histogramHandle(this).exceedsBigInt();
  }

  // The exponentially weighted moving average of recorded values; 0 unless the histogram was
  // created with a `halfLife`.
  get ewmaMean(): number {
    return histogramHandle(this).ewmaMean();
  }

  get ewmaStddev(): number {
    return histogramHandle(this).ewmaStddev();
  }

  // The EWMA of the fraction of values above `threshold`; 0 unless the histogram was created with
  // both `halfLife` and `threshold`.
  get ewmaErrorRate(): number {
    return histogramHandle(this).ewmaErrorRate();
  }

  get kurtosis(): number {
    return histogramHandle(this).kurtosis();
  }

  get skewness(): number {
    return histogramHandle(this).skewness();
  }

  get stddev(): number {
    return histogramHandle(this).stddev();
  }

  percentile(percentile: number): number {
    const handle = histogramHandle(this);
    validatePercentile(percentile);
    return handle.percentile(percentile);
  }

  percentileBigInt(percentile: number): bigint {
    const handle = histogramHandle(this);
    validatePercentile(percentile);
    return handle.percentileBigInt(percentile);
  }

  get percentiles(): Map<number, number> {
    return bucketMap(histogramHandle(this).percentiles());
  }

  get percentilesBigInt(): Map<number, bigint> {
    const { percentiles, values } = histogramHandle(this).percentilesBigInt();
    const map = new Map<number, bigint>();
    for (let i = 0; i < values.length; i++) {
      map.set(percentiles[i] as number, values[i] as bigint);
    }
    return map;
  }

  // The values at the given percentiles, computed in one pass.
  percentilesAt(percentiles: number[]): Map<number, number> {
    const handle = histogramHandle(this);
    validateArray(percentiles, 'percentiles');
    for (let i = 0; i < percentiles.length; i++) {
      validatePercentile(percentiles[i], `percentiles[${i}]`);
    }
    const sorted = [...percentiles].sort((a, b) => a - b);
    const values = handle.percentilesAt(sorted);
    const map = new Map<number, number>();
    for (let i = 0; i < sorted.length; i++) {
      map.set(sorted[i] as number, values[i] as number);
    }
    return map;
  }

  // The probability that a recorded value is at most `value`.
  cdf(value: number): number {
    const handle = histogramHandle(this);
    validateNumber(value, 'value');
    return handle.cdf(value);
  }

  // The probability that a recorded value exceeds `value`.
  ccdf(value: number): number {
    const handle = histogramHandle(this);
    validateNumber(value, 'value');
    return 1 - handle.cdf(value);
  }

  // The number of recorded values in the bucket that `value` falls into.
  countAt(value: number): number {
    const handle = histogramHandle(this);
    validateNumber(value, 'value');
    return handle.countAt(value);
  }

  linearBuckets(stepSize: number): Map<number, number> {
    const handle = histogramHandle(this);
    validateInteger(stepSize, 'stepSize', 1);
    return bucketMap(handle.linearBuckets(stepSize));
  }

  logBuckets(firstBucket: number, base: number): Map<number, number> {
    const handle = histogramHandle(this);
    validateInteger(firstBucket, 'firstBucket', 1);
    validateNumber(base, 'base');
    if (base <= 1) throw new ERR_OUT_OF_RANGE('base', '> 1', base);
    return bucketMap(handle.logBuckets(firstBucket, base));
  }

  // A confidence interval for the mean, from Student's t-distribution.
  meanCI(options: { confidence?: number } = {}): {
    mean: number;
    lower: number;
    upper: number;
  } {
    const handle = histogramHandle(this);
    const [mean, lower, upper] = handle.meanCI(confidenceOption(options)) as [
      number,
      number,
      number,
    ];
    return { __proto__: kNullProtoObject, mean, lower, upper } as never;
  }

  // A confidence interval for a percentile, from the exact binomial method.
  percentileCI(
    percentile: number,
    options: { confidence?: number } = {}
  ): { value: number; lower: number; upper: number } {
    const handle = histogramHandle(this);
    validatePercentile(percentile);
    const [value, lower, upper] = handle.percentileCI(
      percentile,
      confidenceOption(options)
    ) as [number, number, number];
    return { __proto__: kNullProtoObject, value, lower, upper } as never;
  }

  // How fast the error budget of an SLO of `sloTarget` is spent; needs `halfLife` and
  // `threshold`.
  burnRate(sloTarget: number): number {
    const handle = histogramHandle(this);
    validateNumber(sloTarget, 'sloTarget');
    if (Number.isNaN(sloTarget) || sloTarget <= 0 || sloTarget >= 1) {
      throw new ERR_OUT_OF_RANGE('sloTarget', '> 0 && < 1', sloTarget);
    }
    return handle.ewmaErrorRate() / (1 - sloTarget);
  }

  // The Kolmogorov-Smirnov statistic of this histogram and `other`.
  ksTest(other: Histogram): number {
    const handle = histogramHandle(this);
    return handle.ksTest(otherHandle(other));
  }

  welchTest(
    other: Histogram,
    options: { confidence?: number } = {}
  ): {
    tStatistic: number;
    degreesOfFreedom: number;
    pValue: number;
    confidenceInterval: { lower: number; upper: number };
  } {
    const handle = histogramHandle(this);
    const otherH = otherHandle(other);
    const [tStatistic, degreesOfFreedom, pValue, lower, upper] =
      handle.welchTest(otherH, confidenceOption(options)) as [
        number,
        number,
        number,
        number,
        number,
      ];
    return {
      __proto__: kNullProtoObject,
      tStatistic,
      degreesOfFreedom,
      pValue,
      confidenceInterval: { __proto__: kNullProtoObject, lower, upper },
    } as never;
  }

  mannWhitneyTest(other: Histogram): {
    uStatistic: number;
    zScore: number;
    pValue: number;
  } {
    const handle = histogramHandle(this);
    const [uStatistic, zScore, pValue] = handle.mannWhitneyTest(
      otherHandle(other)
    ) as [number, number, number];
    return { __proto__: kNullProtoObject, uStatistic, zScore, pValue } as never;
  }

  // Cohen's d effect size of this histogram against `other`.
  cohensD(other: Histogram): number {
    const handle = histogramHandle(this);
    return handle.cohensD(otherHandle(other));
  }

  // Cliff's delta of this histogram against `other`.
  cliffsD(other: Histogram): number {
    const handle = histogramHandle(this);
    return handle.cliffsD(otherHandle(other));
  }

  // A quantile-respectful density estimate from Harrell-Davis quantiles. Node.js computes it on
  // its thread pool; here it is computed synchronously and the promise is already resolved.
  qrde(options: QrdeOptions = {}): Promise<QrdeResult> {
    const handle = histogramHandle(this);
    validateObject(options, 'options');
    const { bins, probabilities, dequantize = 'hdr', cache = false } = options;
    const boundaries = qrdeProbabilities(bins, probabilities, options);
    validateOneOf(dequantize, 'options.dequantize', kQrdeDequantizationModes);
    validateBoolean(cache, 'options.cache');
    const result = handle.qrde(
      boundaries,
      kQrdeDequantizationModes.indexOf(dequantize)
    );
    return Promise.resolve({
      __proto__: kNullProtoObject,
      probabilities: new Float64Array(boundaries),
      quantiles: new Float64Array(result.quantiles),
      densities: new Float64Array(result.densities),
      count: result.count,
      bucketCount: result.buckets,
      corrections: result.corrections,
      dequantize,
    } as never);
  }

  // An independent copy of this histogram, which cannot be recorded into.
  snapshot(): Histogram {
    return new Histogram(kSkipThrow, histogramHandle(this).snapshot());
  }

  // The values recorded after `other`, an earlier snapshot of this histogram, was taken.
  diff(other: Histogram): Histogram {
    const handle = histogramHandle(this);
    const { histogram, error } = handle.diff(otherHandle(other));
    switch (error) {
      case kDiffIncompatible:
        throw new NodeTypeError(
          'ERR_INVALID_ARG_VALUE',
          'other must have the same configuration as the histogram'
        );
      case kDiffReset:
        throw new NodeError(
          'ERR_INVALID_STATE',
          'Values were removed from the histogram after other was taken'
        );
      case kDiffNotEarlier:
        throw new NodeTypeError(
          'ERR_INVALID_ARG_VALUE',
          'other contains values that are not in the histogram'
        );
    }
    return new Histogram(kSkipThrow, histogram as Handle);
  }

  // The histogram in Node.js's CBOR export format, for `importHistogram()`.
  export(): Uint8Array {
    return histogramHandle(this).export();
  }

  reset(): void {
    histogramHandle(this).reset();
  }

  // The number of times values were removed from the histogram by `reset()` or `subtract()`.
  get resetCount(): number {
    return histogramHandle(this).resetCount();
  }

  toJSON(): Record<string, unknown> {
    return {
      count: this.count,
      min: this.min,
      max: this.max,
      mean: this.mean,
      exceeds: this.exceeds,
      stddev: this.stddev,
      skewness: this.skewness,
      kurtosis: this.kurtosis,
      ewmaMean: this.ewmaMean,
      ewmaStddev: this.ewmaStddev,
      ewmaErrorRate: this.ewmaErrorRate,
      percentiles: Object.fromEntries(this.percentiles),
    };
  }
}

export class RecordableHistogram extends Histogram {
  #recordable = true;

  static {
    isRecordable = (value: unknown): boolean =>
      typeof value === 'object' && value !== null && #recordable in value;
  }

  record(val: number | bigint): void {
    const handle = recordableHandle(this);
    if (typeof val === 'bigint') {
      if (val < 0n || val > kMaxInt64) {
        throw new NodeRangeError('ERR_OUT_OF_RANGE', 'value is out of range');
      }
      handle.recordBigInt(val);
    } else {
      validateInteger(val, 'val', 0);
      handle.record(val);
    }
  }

  // Records the time in nanoseconds since the previous call. The first call records nothing. In
  // Workers, the clock only advances on I/O, so the deltas within one synchronous run are 0.
  recordDelta(): void {
    recordableHandle(this).recordDelta(Date.now());
  }

  // Records `val` with coordinated omission correction: when it exceeds `expectedInterval`,
  // the values a stall kept from being recorded are backfilled at `expectedInterval` steps.
  recordCorrected(
    val: number | bigint,
    expectedInterval: number | bigint
  ): void {
    const handle = recordableHandle(this);
    if (typeof val === 'bigint') {
      if (typeof expectedInterval !== 'bigint') {
        throw new ERR_INVALID_ARG_TYPE(
          'expectedInterval',
          'bigint',
          expectedInterval
        );
      }
      if (val < 0n || val > kMaxInt64) {
        throw new NodeRangeError('ERR_OUT_OF_RANGE', 'value is out of range');
      }
      if (expectedInterval < 1n || expectedInterval > kMaxInt64) {
        throw new NodeRangeError(
          'ERR_OUT_OF_RANGE',
          'expected_interval is out of range'
        );
      }
      handle.recordCorrectedBigInt(val, expectedInterval);
    } else {
      validateInteger(val, 'val', 0);
      validateInteger(expectedInterval, 'expectedInterval', 1);
      handle.recordCorrected(val, expectedInterval);
    }
  }

  add(other: RecordableHistogram): void {
    const handle = recordableHandle(this);
    handle.add(recordableArgument(other));
  }

  // Subtracts the counts of `other`, clamping at zero.
  subtract(other: RecordableHistogram): void {
    const handle = recordableHandle(this);
    handle.subtract(recordableArgument(other));
  }
}

function recordableArgument(other: unknown): Handle {
  if (!isRecordable(other)) {
    throw new ERR_INVALID_ARG_TYPE('other', 'RecordableHistogram', other);
  }
  return histogramHandle(other);
}

// A recordable histogram from the data of `histogram.export()`.
export function importHistogram(data: Uint8Array): RecordableHistogram {
  if (!isUint8Array(data)) {
    throw new ERR_INVALID_ARG_TYPE('data', 'Uint8Array', data);
  }
  const handle = histogramImpl.import(data);
  if (handle === undefined) {
    throw new NodeTypeError(
      'ERR_INVALID_ARG_VALUE',
      'Invalid histogram export data'
    );
  }
  return new RecordableHistogram(kSkipThrow, handle);
}

export interface CreateHistogramOptions {
  lowest?: number | bigint;
  highest?: number | bigint;
  figures?: number;
  halfLife?: number;
  threshold?: number;
}

function validateBound(
  value: unknown,
  name: string
): asserts value is number | bigint {
  if (typeof value !== 'bigint') {
    validateInteger(value, name, 1, Number.MAX_SAFE_INTEGER);
  } else if (value < 1n || value > kMaxInt64) {
    throw new ERR_OUT_OF_RANGE(name, `>= 1n && <= ${kMaxInt64}n`, value);
  }
}

function validateHistogramOptions(
  lowest: unknown,
  highest: unknown,
  figures: unknown
): asserts lowest is number | bigint {
  validateBound(lowest, 'options.lowest');
  validateBound(highest, 'options.highest');
  const minimumHighest = 2n * BigInt(lowest);
  if (BigInt(highest) < minimumHighest) {
    throw new ERR_OUT_OF_RANGE(
      'options.highest',
      `>= 2 * options.lowest (${minimumHighest}n)`,
      highest
    );
  }
  validateInteger(figures, 'options.figures', 1, 5);
}

export function createHistogram(
  options: CreateHistogramOptions = {}
): RecordableHistogram {
  validateObject(options, 'options');
  const {
    lowest = 1,
    highest = Number.MAX_SAFE_INTEGER,
    figures = 3,
    halfLife = 0,
    threshold = 0,
  } = options;
  validateHistogramOptions(lowest, highest, figures);
  validateNumber(halfLife, 'options.halfLife');
  if (halfLife < 0) {
    throw new ERR_OUT_OF_RANGE('options.halfLife', '>= 0', halfLife);
  }
  validateNumber(threshold, 'options.threshold');
  if (threshold < 0) {
    throw new ERR_OUT_OF_RANGE('options.threshold', '>= 0', threshold);
  }
  return new RecordableHistogram(
    kSkipThrow,
    new histogramImpl.Histogram(lowest, highest, figures, halfLife, threshold)
  );
}
