'use strict';

// Helpers shared by the webstreams modules. Leaf module: requires nothing.

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  QueuingStrategy,
  Transformer,
  UnderlyingSink,
  UnderlyingSource,
} from './types';

const {
  BigInt,
  NumberIsFinite,
  NumberIsNaN,
  ObjectFreeze,
  PromiseWithResolvers,
  RangeError,
  TypeError,
  uncurryThis,
} = primordials;

const { isPromise, markPromiseHandled } = utils;

function isActualObject(value: unknown): value is object {
  return value != null && typeof value === 'object';
}

// What an omitted (or null) dictionary argument stands for. Null-prototype:
// WebIDL reads nothing for an omitted dictionary, so neither may we.
const kNullProtoObject: object = ObjectFreeze({ __proto__: null });

const kEnumerable: PropertyDescriptor = ObjectFreeze({
  __proto__: kNullProtoObject,
  enumerable: true,
});

// WebIDL "a promise resolved with" a value: a new promise resolved with it.
// For a promise or thenable, its reactions run one microtask after the
// value's (two if the value settles before the resolution job subscribes
// to it), where PromiseResolve adopts a native promise as is. The readable
// and writable controllers settle their start() results this way, as the
// transformer's cancel and flush are (transform.ts); that timing is
// observable, as in WPT transform-streams/cancel.any.js.
function promiseResolvedWith(value: unknown): Promise<void> {
  const { promise, resolve } =
    PromiseWithResolvers() as PromiseWithResolversType<void>;
  resolve(value as void);
  return promise;
}

// A promise slot (a writer's ready and closed promises, a reader's closed
// promise) holds a still-pending resolvers record, or the settled promise
// it became. The two are told apart with the native isPromise: a
// duck-typed `typeof slot.resolve` would read Object.prototype on the
// promise.
export type PromiseSlot = Promise<void> | PromiseWithResolversType<void>;

// A new resolvers record whose promise is marked handled, so nothing need
// ever look at its rejection. A slot starts pending as one.
function newHandledPending(): PromiseWithResolversType<void> {
  const pending = PromiseWithResolvers() as PromiseWithResolversType<void>;
  markPromiseHandled(pending.promise);
  return pending;
}

// A promise rejected with error, marked handled before it rejects: a
// promise from PromiseReject would be reported as unhandled first.
function newHandledRejected(error: unknown): Promise<void> {
  const pending = newHandledPending();
  pending.reject(error);
  return pending.promise;
}

function slotPromise(slot: PromiseSlot): Promise<void> {
  return isPromise(slot) ? slot : slot.promise;
}

// Resolves a pending slot's promise; a settled slot is returned as it is.
function slotResolved(slot: PromiseSlot): Promise<void> {
  if (isPromise(slot)) return slot;
  slot.resolve();
  return slot.promise;
}

// Rejects a pending slot's promise with error; a settled slot gives way to
// a new handled promise rejected with error, as the spec's writer
// EnsureReadyPromiseRejected and EnsureClosedPromiseRejected do.
function slotRejected(slot: PromiseSlot, error: unknown): Promise<void> {
  if (isPromise(slot)) return newHandledRejected(error);
  slot.reject(error);
  return slot.promise;
}

function toStringTagProperty(tag: string): PropertyDescriptor {
  return {
    __proto__: kNullProtoObject,
    value: tag,
    writable: false,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor;
}

const kMaxUint64 = 0xffff_ffff_ffff_ffffn;

// Converts the non-standard expectedLength extension: the total bytes a
// source or a transformer promises, or a FixedLengthStream's length. It
// converts as the C++ implementation converts a uint64_t, so the two accept
// the same values and agree on what they mean. undefined means no length. A
// bigint must fit in a uint64. Anything else goes through ToNumber (so '5' is
// 5, null is 0, and an object's valueOf runs), must be finite and
// non-negative, and is truncated (1.5 is 1). Every failure is a TypeError.
// One difference: C++'s range check lets the number 2 ** 64 through to a cast
// whose result is undefined; here it is a TypeError.
function normalizeExpectedLength(value: unknown): bigint | undefined {
  if (value === undefined) return undefined;
  if (typeof value === 'bigint') {
    if (value < 0n || value > kMaxUint64) {
      throw new TypeError('expectedLength must fit in a uint64');
    }
    return value;
  }
  // Unary plus is ToNumber. Number() would differ for an object whose
  // valueOf returns a bigint: it converts that, where ToNumber throws.
  const length = +(value as number);
  if (!NumberIsFinite(length) || length < 0 || length >= 2 ** 64) {
    throw new TypeError(
      'expectedLength must be a non-negative number that fits in a uint64'
    );
  }
  // Truncates, as length is finite and non-negative.
  return BigInt(length - (length % 1));
}

// Converts a source's autoAllocateChunkSize as WebIDL converts an
// [EnforceRange] unsigned long long: ToNumber, then NaN, ±Infinity and a
// result outside [0, 2^53 - 1] are a TypeError, and a fraction truncates.
// undefined means none. Zero passes: the byte controller rejects it itself,
// after the highWaterMark, as the spec does.
function convertAutoAllocateChunkSize(value: unknown): number | undefined {
  if (value === undefined) return undefined;
  // Unary plus is ToNumber (see normalizeExpectedLength).
  const size = +(value as number);
  // The truncated value lies in [0, 2^53 - 1] exactly when the value lies
  // in (-1, 2^53). NaN fails both comparisons.
  if (!(size > -1 && size < 2 ** 53)) {
    throw new TypeError(
      'autoAllocateChunkSize must be a finite number from 0 to 2^53 - 1'
    );
  }
  // Truncates, and turns -0 into 0.
  return size - (size % 1);
}

// WebIDL's check that a value is an object, a function included: null and
// every other primitive throw. Nothing is read. It converts an argument typed
// `object` (an underlying source or sink, a transformer), and a dictionary
// with a required member, which undefined and null cannot be.
function requireObject(value: unknown, message: string): void {
  if (!isActualObject(value) && typeof value !== 'function') {
    throw new TypeError(message);
  }
}

// WebIDL conversion of an optional dictionary argument (a strategy, an
// options bag), as in C++: undefined and null mean an empty dictionary, and
// anything else must pass requireObject.
function toDictionary(value: unknown, message: string): object {
  if (value == null) return kNullProtoObject;
  requireObject(value, message);
  return value as object;
}

function toStrategyDictionary(value: unknown): QueuingStrategy<unknown> {
  return toDictionary(
    value,
    'The queuing strategy must be an object.'
  ) as QueuingStrategy<unknown>;
}

// A strategy argument as WebIDL converts a QueuingStrategy dictionary: each
// member read once, in lexicographic order. highWaterMark has been through
// ToNumber; size is a function. An absent member is undefined.
interface QueuingStrategyDict {
  highWaterMark: number | undefined;
  size: ((chunk: unknown) => number) | undefined;
}

function convertQueuingStrategy(value: unknown): QueuingStrategyDict {
  const strategy = toStrategyDictionary(value);
  const rawHighWaterMark: unknown = strategy.highWaterMark;
  // Unary plus is ToNumber (see normalizeExpectedLength).
  const highWaterMark =
    rawHighWaterMark === undefined ? undefined : +(rawHighWaterMark as number);
  const size: unknown = strategy.size;
  if (size !== undefined && typeof size !== 'function') {
    throw new TypeError('strategy.size must be a function');
  }
  return {
    __proto__: kNullProtoObject,
    highWaterMark,
    size,
  } as QueuingStrategyDict;
}

// ToNumber and the range check of the spec's ExtractHighWaterMark: NaN and
// negative numbers are a RangeError; Infinity and fractions are kept.
// Converting a number again has no side effects.
function convertHighWaterMark(value: unknown): number {
  const highWaterMark = +(value as number);
  if (NumberIsNaN(highWaterMark) || highWaterMark < 0) {
    throw new RangeError('Invalid highWaterMark');
  }
  return highWaterMark;
}

// The spec's ExtractHighWaterMark, on a converted strategy.
function extractHighWaterMark(
  strategy: QueuingStrategyDict,
  defaultHighWaterMark: number
): number {
  const { highWaterMark } = strategy;
  return highWaterMark === undefined
    ? defaultHighWaterMark
    : convertHighWaterMark(highWaterMark);
}

// The spec's ExtractSizeAlgorithm, on a converted strategy: size() runs with
// an undefined receiver and the chunk as its only argument. Without one,
// every chunk counts 1.
function extractSizeAlgorithm<T>(
  strategy: QueuingStrategyDict
): (chunk: T) => number {
  const { size } = strategy;
  if (size === undefined) return () => 1;
  const callSize = uncurryThis(size);
  return (chunk: T) => callSize(undefined, chunk);
}

// WebIDL's conversion of an optional callback member of a source, sink or
// transformer: undefined means none, and anything else must be callable.
// `name` names the member in the TypeError.
function convertCallback<F>(value: F | undefined, name: string): F | undefined {
  if (value !== undefined && typeof value !== 'function') {
    throw new TypeError(`${name} must be a function`);
  }
  return value;
}

// An underlying sink as WebIDL converts it to an UnderlyingSink dictionary,
// before the constructor checks anything: every member read once, in
// lexicographic order, each callback checked. type is kept as read, for the
// WritableStream constructor to check. The callbacks still run with the
// sink itself as their receiver.
export interface UnderlyingSinkDict<W> {
  abort: UnderlyingSink<W>['abort'];
  close: UnderlyingSink<W>['close'];
  start: UnderlyingSink<W>['start'];
  type: unknown;
  write: UnderlyingSink<W>['write'];
}

function convertUnderlyingSink<W>(
  sink: UnderlyingSink<W>
): UnderlyingSinkDict<W> {
  // The members are read in the order listed.
  return {
    __proto__: kNullProtoObject,
    abort: convertCallback(sink.abort, 'underlyingSink.abort'),
    close: convertCallback(sink.close, 'underlyingSink.close'),
    start: convertCallback(sink.start, 'underlyingSink.start'),
    type: (sink as { type?: unknown }).type,
    write: convertCallback(sink.write, 'underlyingSink.write'),
  } as UnderlyingSinkDict<W>;
}

// A transformer as WebIDL converts it to a Transformer dictionary, before
// the constructor checks anything: every member read once, in
// lexicographic order, each callback checked, and the expectedLength
// extension converted. readableType and writableType are kept as read, for
// the TransformStream constructor to check. The callbacks still run with
// the transformer itself as their receiver.
interface TransformerDict<I, O> {
  cancel: Transformer<I, O>['cancel'];
  expectedLength: bigint | undefined;
  flush: Transformer<I, O>['flush'];
  readableType: unknown;
  start: Transformer<I, O>['start'];
  transform: Transformer<I, O>['transform'];
  writableType: unknown;
}

function convertTransformer<I, O>(
  transformer: Transformer<I, O>
): TransformerDict<I, O> {
  const members = transformer as {
    expectedLength?: unknown;
    readableType?: unknown;
    writableType?: unknown;
  };
  // The members are read in the order listed.
  return {
    __proto__: kNullProtoObject,
    cancel: convertCallback(transformer.cancel, 'transformer.cancel'),
    expectedLength: normalizeExpectedLength(members.expectedLength),
    flush: convertCallback(transformer.flush, 'transformer.flush'),
    readableType: members.readableType,
    start: convertCallback(transformer.start, 'transformer.start'),
    transform: convertCallback(transformer.transform, 'transformer.transform'),
    writableType: members.writableType,
  } as TransformerDict<I, O>;
}

// WebIDL's conversion of the ReadableStreamType enum, whose one value is
// 'bytes': ToString, so an object's toString() runs, then any other string
// is a TypeError. undefined means absent.
function convertReadableStreamType(value: unknown): 'bytes' | undefined {
  if (value === undefined) return undefined;
  const type = `${value as string}`;
  if (type !== 'bytes') {
    throw new TypeError(`Invalid underlying source type: ${type}`);
  }
  return type;
}

// An underlying source as WebIDL converts it to an UnderlyingSource
// dictionary, before the constructor checks anything: every member read
// once, in lexicographic order, each callback checked, type converted as
// its enum, the expectedLength extension converted, and
// autoAllocateChunkSize converted on a default source too, although only a
// byte source uses it. The callbacks still run with the source itself as
// their receiver.
// S narrows the callbacks to one kind of source, once type is known.
export interface UnderlyingSourceDict<
  R,
  S extends UnderlyingSource<R> = UnderlyingSource<R>,
> {
  autoAllocateChunkSize: number | undefined;
  cancel: S['cancel'];
  expectedLength: bigint | undefined;
  pull: S['pull'];
  start: S['start'];
  type: 'bytes' | undefined;
}

function convertUnderlyingSource<R>(
  source: UnderlyingSource<R>
): UnderlyingSourceDict<R> {
  const members = source as {
    autoAllocateChunkSize?: unknown;
    expectedLength?: unknown;
    type?: unknown;
  };
  // The members are read in the order listed.
  return {
    __proto__: kNullProtoObject,
    autoAllocateChunkSize: convertAutoAllocateChunkSize(
      members.autoAllocateChunkSize
    ),
    cancel: convertCallback(source.cancel, 'underlyingSource.cancel'),
    expectedLength: normalizeExpectedLength(members.expectedLength),
    pull: convertCallback(source.pull, 'underlyingSource.pull'),
    start: convertCallback(source.start, 'underlyingSource.start'),
    type: convertReadableStreamType(members.type),
  } as UnderlyingSourceDict<R>;
}

export type CommonHelpers = {
  isActualObject: typeof isActualObject;
  kNullProtoObject: typeof kNullProtoObject;
  kEnumerable: typeof kEnumerable;
  toStringTagProperty: typeof toStringTagProperty;
  promiseResolvedWith: typeof promiseResolvedWith;
  newHandledPending: typeof newHandledPending;
  newHandledRejected: typeof newHandledRejected;
  slotPromise: typeof slotPromise;
  slotResolved: typeof slotResolved;
  slotRejected: typeof slotRejected;
  convertCallback: typeof convertCallback;
  normalizeExpectedLength: typeof normalizeExpectedLength;
  requireObject: typeof requireObject;
  toDictionary: typeof toDictionary;
  toStrategyDictionary: typeof toStrategyDictionary;
  convertQueuingStrategy: typeof convertQueuingStrategy;
  convertHighWaterMark: typeof convertHighWaterMark;
  extractHighWaterMark: typeof extractHighWaterMark;
  extractSizeAlgorithm: typeof extractSizeAlgorithm;
  convertUnderlyingSink: typeof convertUnderlyingSink;
  convertTransformer: typeof convertTransformer;
  convertUnderlyingSource: typeof convertUnderlyingSource;
};

module.exports = ObjectFreeze({
  isActualObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
  promiseResolvedWith,
  newHandledPending,
  newHandledRejected,
  slotPromise,
  slotResolved,
  slotRejected,
  convertCallback,
  normalizeExpectedLength,
  requireObject,
  toDictionary,
  toStrategyDictionary,
  convertQueuingStrategy,
  convertHighWaterMark,
  extractHighWaterMark,
  extractSizeAlgorithm,
  convertUnderlyingSink,
  convertTransformer,
  convertUnderlyingSource,
});
