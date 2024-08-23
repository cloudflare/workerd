// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { default as zlibUtil, type ZlibOptions } from 'node-internal:zlib';
import { Buffer, kMaxLength } from 'node-internal:internal_buffer';
import {
  checkRangesOrGetDefault,
  checkFiniteNumber,
} from 'node-internal:validators';
import {
  ERR_OUT_OF_RANGE,
  ERR_BUFFER_TOO_LARGE,
  ERR_INVALID_ARG_TYPE,
  NodeError,
} from 'node-internal:internal_errors';
import { Transform, type DuplexOptions } from 'node-internal:streams_transform';
import { eos as finished } from 'node-internal:streams_util';
import {
  isArrayBufferView,
  isAnyArrayBuffer,
} from 'node-internal:internal_types';

// Explicitly import `ok()` to avoid typescript error requiring every name in the call target to
// be annotated with an explicit type annotation.
import assert, { ok } from 'node-internal:internal_assert';

const {
  CONST_INFLATE,
  CONST_GUNZIP,
  CONST_GZIP,
  CONST_UNZIP,
  CONST_Z_DEFAULT_CHUNK,
  CONST_Z_DEFAULT_STRATEGY,
  CONST_Z_DEFAULT_MEMLEVEL,
  CONST_Z_DEFAULT_WINDOWBITS,
  CONST_Z_DEFAULT_COMPRESSION,
  CONST_Z_FIXED,
  CONST_Z_MAX_LEVEL,
  CONST_Z_MAX_MEMLEVEL,
  CONST_Z_MAX_WINDOWBITS,
  CONST_Z_MIN_LEVEL,
  CONST_Z_MIN_MEMLEVEL,
  CONST_Z_SYNC_FLUSH,
  CONST_Z_NO_FLUSH,
  CONST_Z_BLOCK,
  CONST_Z_MIN_CHUNK,
  CONST_Z_PARTIAL_FLUSH,
  CONST_Z_FULL_FLUSH,
  CONST_Z_FINISH,
  CONST_BROTLI_ENCODE,
  CONST_BROTLI_DECODE,
  CONST_BROTLI_OPERATION_PROCESS,
  CONST_BROTLI_OPERATION_EMIT_METADATA,
} = zlibUtil;

export const owner_symbol = Symbol('owner');

const FLUSH_BOUND_IDX_NORMAL: number = 0;
const FLUSH_BOUND_IDX_BROTLI: number = 1;
const FLUSH_BOUND: [[number, number], [number, number]] = [
  [CONST_Z_NO_FLUSH, CONST_Z_BLOCK],
  [CONST_BROTLI_OPERATION_PROCESS, CONST_BROTLI_OPERATION_EMIT_METADATA],
];

const kFlushFlag = Symbol('kFlushFlag');
const kError = Symbol('kError');

function processCallback(this: zlibUtil.ZlibStream): void {
  // This callback's context (`this`) is the `_handle` (ZCtx) object. It is
  // important to null out the values once they are no longer needed since
  // `_handle` can stay in memory long after the buffer is needed.
  // eslint-disable-next-line @typescript-eslint/no-this-alias
  const handle = this;
  const self = this[owner_symbol];
  ok(self, 'Owner symbol should exist');
  const state = self._writeState;

  if (self.destroyed) {
    this.buffer = null;
    this.cb();
    return;
  }

  const availOutAfter = state[0] as number;
  const availInAfter = state[1] as number;

  const inDelta = handle.availInBefore - availInAfter;
  self.bytesWritten += inDelta;

  const have = handle.availOutBefore - availOutAfter;
  let streamBufferIsFull = false;
  if (have > 0) {
    const out = self._outBuffer.slice(self._outOffset, self._outOffset + have);
    self._outOffset += have;
    streamBufferIsFull = !self.push(out);
  } else {
    assert.strictEqual(have, 0, 'have should not go down');
  }

  if (self.destroyed) {
    this.cb();
    return;
  }

  // Exhausted the output buffer, or used all the input create a new one.
  if (availOutAfter === 0 || self._outOffset >= self._chunkSize) {
    handle.availOutBefore = self._chunkSize;
    self._outOffset = 0;
    self._outBuffer = Buffer.allocUnsafe(self._chunkSize);
  }

  if (availOutAfter === 0) {
    // Not actually done. Need to reprocess.
    // Also, update the availInBefore to the availInAfter value,
    // so that if we have to hit it a third (fourth, etc.) time,
    // it'll have the correct byte counts.
    handle.inOff += inDelta;
    handle.availInBefore = availInAfter;

    if (!streamBufferIsFull) {
      ok(this.buffer, 'Buffer should not have been null');
      this.write(
        handle.flushFlag,
        this.buffer, // in
        handle.inOff, // in_off
        handle.availInBefore, // in_len
        self._outBuffer, // out
        self._outOffset, // out_off
        self._chunkSize
      ); // out_len
    } else {
      // eslint-disable-next-line @typescript-eslint/unbound-method
      const oldRead = self._read;
      self._read = (n): void => {
        ok(this.buffer, 'Buffer should not have been null');
        self._read = oldRead;
        this.write(
          handle.flushFlag,
          this.buffer, // in
          handle.inOff, // in_off
          handle.availInBefore, // in_len
          self._outBuffer, // out
          self._outOffset, // out_off
          self._chunkSize
        ); // out_len
        self._read(n);
      };
    }
    return;
  }

  if (availInAfter > 0) {
    // If we have more input that should be written, but we also have output
    // space available, that means that the compression library was not
    // interested in receiving more data, and in particular that the input
    // stream has ended early.
    // This applies to streams where we don't check data past the end of
    // what was consumed; that is, everything except Gunzip/Unzip.
    self.push(null);
  }

  // Finished with the chunk.
  this.buffer = null;
  this.cb();
}

// If a flush is scheduled while another flush is still pending, a way to figure
// out which one is the "stronger" flush is needed.
// This is currently only used to figure out which flush flag to use for the
// last chunk.
// Roughly, the following holds:
// Z_NO_FLUSH (< Z_TREES) < Z_BLOCK < Z_PARTIAL_FLUSH <
//     Z_SYNC_FLUSH < Z_FULL_FLUSH < Z_FINISH
const flushiness: number[] = [];
const kFlushFlagList: number[] = [
  CONST_Z_NO_FLUSH,
  CONST_Z_BLOCK,
  CONST_Z_PARTIAL_FLUSH,
  CONST_Z_SYNC_FLUSH,
  CONST_Z_FULL_FLUSH,
  CONST_Z_FINISH,
];
for (let i = 0; i < kFlushFlagList.length; i++) {
  flushiness[kFlushFlagList[i] as number] = i;
}

type BufferWithFlushFlag = Buffer & { [kFlushFlag]: number };

// Set up a list of 'special' buffers that can be written using .write()
// from the .flush() code as a way of introducing flushing operations into the
// write sequence.
const kFlushBuffers: BufferWithFlushFlag[] = [];
{
  const dummyArrayBuffer = new ArrayBuffer(0);
  for (const flushFlag of kFlushFlagList) {
    const buf = Buffer.from(dummyArrayBuffer) as BufferWithFlushFlag;
    buf[kFlushFlag] = flushFlag;
    kFlushBuffers[flushFlag] = buf;
  }
}

function zlibOnError(
  this: zlibUtil.ZlibStream,
  errno: number,
  code: string,
  message: string
): void {
  const self = this[owner_symbol];
  ok(self, 'Owner symbol should exist');
  const error = new NodeError(code, message);
  // @ts-expect-error Err number is expected.
  error.errno = errno;
  self.destroy(error);
  self[kError] = error;
}

function processChunkSync(
  self: Zlib,
  chunk: Buffer,
  flushFlag: number
): Buffer | Uint8Array {
  let availInBefore = chunk.byteLength;
  let availOutBefore = self._chunkSize - self._outOffset;
  let inOff = 0;
  let availOutAfter;
  let availInAfter;

  const buffers: (Buffer | Uint8Array)[] = [];
  let nread = 0;
  let inputRead = 0;
  const state = self._writeState;
  const handle = self._handle;
  let buffer = self._outBuffer;
  let offset = self._outOffset;
  const chunkSize = self._chunkSize;

  let error;
  self.on('error', function onError(er) {
    error = er;
  });

  while (true) {
    ok(handle, 'Handle should have been defined');
    handle.writeSync(
      flushFlag,
      chunk, // in
      inOff, // in_off
      availInBefore, // in_len
      buffer, // out
      offset, // out_off
      availOutBefore // out_len
    );
    if (error) throw error;
    else if (self[kError]) throw self[kError];

    [availOutAfter, availInAfter] = state as unknown as [number, number];

    const inDelta = availInBefore - availInAfter;
    inputRead += inDelta;

    const have = availOutBefore - availOutAfter;
    if (have > 0) {
      const out = buffer.slice(offset, offset + have);
      offset += have;
      buffers.push(out);
      nread += out.byteLength;

      if (nread > self._maxOutputLength) {
        _close(self);
        throw new ERR_BUFFER_TOO_LARGE(self._maxOutputLength);
      }
    } else {
      assert.strictEqual(have, 0, 'have should not go down');
    }

    // Exhausted the output buffer, or used all the input create a new one.
    if (availOutAfter === 0 || offset >= chunkSize) {
      availOutBefore = chunkSize;
      offset = 0;
      buffer = Buffer.allocUnsafe(chunkSize);
    }

    if (availOutAfter === 0) {
      // Not actually done. Need to reprocess.
      // Also, update the availInBefore to the availInAfter value,
      // so that if we have to hit it a third (fourth, etc.) time,
      // it'll have the correct byte counts.
      inOff += inDelta;
      availInBefore = availInAfter;
    } else {
      break;
    }
  }

  self.bytesWritten = inputRead;
  _close(self);

  if (nread === 0) return Buffer.alloc(0);

  return buffers.length === 1
    ? (buffers[0] as Buffer)
    : Buffer.concat(buffers, nread);
}

function _close(engine: ZlibBase): void {
  engine._handle?.close();
  engine._handle = null;
}

type ZlibDefaultOptions = {
  flush: number;
  finishFlush: number;
  fullFlush: number;
};

const zlibDefaultOptions = {
  flush: CONST_Z_NO_FLUSH,
  finishFlush: CONST_Z_FINISH,
  fullFlush: CONST_Z_FULL_FLUSH,
};

export class ZlibBase extends Transform {
  public bytesWritten: number = 0;

  public _maxOutputLength: number;
  public _outBuffer: Buffer;
  public _outOffset: number = 0;
  public _chunkSize: number;
  public _defaultFlushFlag: number;
  public _finishFlushFlag: number;
  public _defaultFullFlushFlag: number;
  public _info: unknown;
  public _handle: zlibUtil.ZlibStream | null = null;
  public _writeState = new Uint32Array(2);

  public [kError]: NodeError | undefined;

  public constructor(
    opts: ZlibOptions & DuplexOptions,
    mode: number,
    handle: zlibUtil.ZlibStream,
    { flush, finishFlush, fullFlush }: ZlibDefaultOptions = zlibDefaultOptions
  ) {
    let chunkSize = CONST_Z_DEFAULT_CHUNK;
    let maxOutputLength = kMaxLength;

    let flushBoundIdx;
    if (mode !== CONST_BROTLI_ENCODE && mode !== CONST_BROTLI_DECODE) {
      flushBoundIdx = FLUSH_BOUND_IDX_NORMAL;
    } else {
      flushBoundIdx = FLUSH_BOUND_IDX_BROTLI;
    }

    if (opts) {
      if (opts.chunkSize != null) {
        chunkSize = opts.chunkSize;
      }
      if (!checkFiniteNumber(chunkSize, 'options.chunkSize')) {
        chunkSize = CONST_Z_DEFAULT_CHUNK;
      } else if (chunkSize < CONST_Z_MIN_CHUNK) {
        throw new ERR_OUT_OF_RANGE(
          'options.chunkSize',
          `>= ${CONST_Z_MIN_CHUNK}`,
          chunkSize
        );
      }

      flush = checkRangesOrGetDefault(
        opts.flush,
        'options.flush',
        FLUSH_BOUND[flushBoundIdx]?.[0] as number,
        FLUSH_BOUND[flushBoundIdx]?.[1] as number,
        flush
      );

      finishFlush = checkRangesOrGetDefault(
        opts.finishFlush,
        'options.finishFlush',
        FLUSH_BOUND[flushBoundIdx]?.[0] as number,
        FLUSH_BOUND[flushBoundIdx]?.[1] as number,
        finishFlush
      );

      maxOutputLength = checkRangesOrGetDefault(
        opts.maxOutputLength,
        'options.maxOutputLength',
        1,
        kMaxLength,
        kMaxLength
      );

      if (opts.encoding || opts.objectMode || opts.writableObjectMode) {
        opts = { ...opts };
        opts.encoding = undefined;
        opts.objectMode = false;
        opts.writableObjectMode = false;
      }
    }

    // @ts-expect-error TODO: Find a way to avoid having "unknown"
    super({ autoDestroy: true, ...opts } as unknown);

    // Error handler by processCallback() and zlibOnError()
    handle.setErrorHandler(zlibOnError.bind(handle));
    handle[owner_symbol] = this as never;
    this._handle = handle;
    this._outBuffer = Buffer.allocUnsafe(chunkSize);
    this._outOffset = 0;
    this._chunkSize = chunkSize;
    this._defaultFlushFlag = flush;
    this._finishFlushFlag = finishFlush;
    this._defaultFullFlushFlag = fullFlush;
    this._info = opts && opts.info;
    this._maxOutputLength = maxOutputLength;
  }

  public get _closed(): boolean {
    return this._handle == null;
  }

  // @deprecated Use `bytesWritten` instead.
  public get bytesRead(): number {
    return this.bytesWritten;
  }

  // @deprecated Use `bytesWritten` instead.
  public set bytesRead(value: number) {
    this.bytesWritten = value;
  }

  public reset(): void {
    ok(this._handle, 'zlib binding closed');
    this._handle.reset();
  }

  // This is the _flush function called by the transform class,
  // internally, when the last chunk has been written.
  public override _flush(callback: () => void): void {
    this._transform(Buffer.alloc(0), 'utf8', callback);
  }

  // Force Transform compat behavior.
  public override _final(callback: () => void): void {
    callback();
  }

  public flush(kind: number, callback: () => void): void;
  public flush(callback?: () => void): void;
  public flush(
    kind?: number | (() => void),
    callback: (() => void) | undefined = undefined
  ): void {
    if (typeof kind === 'function' || (kind == null && !callback)) {
      callback = kind as (() => void) | undefined;
      kind = this._defaultFlushFlag;
    }

    if (this.writableFinished) {
      if (callback) {
        /* eslint-disable-next-line @typescript-eslint/no-unsafe-call */
        queueMicrotask(callback);
      }
    } else if (this.writableEnded) {
      if (callback) {
        this.once('end', callback);
      }
    } else {
      this.write(kFlushBuffers[kind as number], 'utf8', callback);
    }
  }

  public close(callback?: () => void): void {
    if (callback) {
      finished(this, callback);
    }
    this.destroy();
  }

  public override _destroy<T extends Error>(
    err: T,
    callback: (err: T) => never
  ): void {
    _close(this);
    callback(err);
  }

  public override _transform(
    chunk: Buffer & { [kFlushFlag]?: number },
    _encoding: BufferEncoding,
    cb: () => void
  ): void {
    let flushFlag = this._defaultFlushFlag;
    // We use a 'fake' zero-length chunk to carry information about flushes from
    // the public API to the actual stream implementation.
    if (typeof chunk[kFlushFlag] === 'number') {
      flushFlag = chunk[kFlushFlag];
    }

    // For the last chunk, also apply `_finishFlushFlag`.
    if (this.writableEnded && this.writableLength === chunk.byteLength) {
      flushFlag = maxFlush(flushFlag, this._finishFlushFlag);
    }
    this.#processChunk(chunk, flushFlag, cb);
  }

  // This function is left for backwards compatibility.
  public _processChunk(
    chunk: Buffer,
    flushFlag: number,
    cb?: () => void
  ): Buffer | Uint8Array | undefined {
    if (cb != null && typeof cb === 'function') {
      this.#processChunk(chunk, flushFlag, cb);
      return;
    }
    return processChunkSync(this as never, chunk, flushFlag);
  }

  #processChunk(chunk: Buffer, flushFlag: number, cb: () => void): void {
    if (!this._handle) {
      /* eslint-disable-next-line @typescript-eslint/no-unsafe-call */
      queueMicrotask(cb);
      return;
    }

    this._handle.buffer = chunk;
    this._handle.cb = cb;
    this._handle.availOutBefore = this._chunkSize - this._outOffset;
    this._handle.availInBefore = chunk.byteLength;
    this._handle.inOff = 0;
    this._handle.flushFlag = flushFlag;

    this._handle.write(
      flushFlag,
      chunk, // in
      0, // in_off
      this._handle.availInBefore, // in_len
      this._outBuffer, // out
      this._outOffset, // out_off
      this._handle.availOutBefore // out_len
    );
  }
}

function maxFlush(a: number, b: number): number {
  return (flushiness[a] as number) > (flushiness[b] as number) ? a : b;
}

export class Zlib extends ZlibBase {
  public _level = CONST_Z_DEFAULT_COMPRESSION;
  public _strategy = CONST_Z_DEFAULT_STRATEGY;

  public constructor(options: ZlibOptions | null | undefined, mode: number) {
    let windowBits = CONST_Z_DEFAULT_WINDOWBITS;
    let level = CONST_Z_DEFAULT_COMPRESSION;
    let memLevel = CONST_Z_DEFAULT_MEMLEVEL;
    let strategy = CONST_Z_DEFAULT_STRATEGY;
    let dictionary: ZlibOptions['dictionary'];

    if (options != null) {
      // Special case:
      // - Compression: 0 is an invalid case.
      // - Decompression: 0 indicates zlib to use the window size in the header of the compressed stream.
      if (
        (options.windowBits == null || options.windowBits === 0) &&
        (mode === CONST_INFLATE ||
          mode === CONST_GUNZIP ||
          mode === CONST_UNZIP)
      ) {
        windowBits = 0;
      } else {
        // `{ windowBits: 8 }` is valid for DEFLATE but not for GZIP.
        const min =
          zlibUtil.CONST_Z_MIN_WINDOWBITS + (mode === CONST_GZIP ? 1 : 0);
        windowBits = checkRangesOrGetDefault(
          options.windowBits,
          'options.windowBits',
          min,
          CONST_Z_MAX_WINDOWBITS,
          CONST_Z_DEFAULT_WINDOWBITS
        );
      }

      level = checkRangesOrGetDefault(
        options.level,
        'options.level',
        CONST_Z_MIN_LEVEL,
        CONST_Z_MAX_LEVEL,
        CONST_Z_DEFAULT_COMPRESSION
      );
      memLevel = checkRangesOrGetDefault(
        options.memLevel,
        'options.memLevel',
        CONST_Z_MIN_MEMLEVEL,
        CONST_Z_MAX_MEMLEVEL,
        CONST_Z_DEFAULT_MEMLEVEL
      );
      strategy = checkRangesOrGetDefault(
        options.strategy,
        'options.strategy',
        CONST_Z_DEFAULT_STRATEGY,
        CONST_Z_FIXED,
        CONST_Z_DEFAULT_STRATEGY
      );
      dictionary = options.dictionary;

      if (dictionary !== undefined && !isArrayBufferView(dictionary)) {
        if (isAnyArrayBuffer(dictionary)) {
          dictionary = Buffer.from(dictionary);
        } else {
          throw new ERR_INVALID_ARG_TYPE(
            'options.dictionary',
            ['Buffer', 'TypedArray', 'DataView', 'ArrayBuffer'],
            dictionary
          );
        }
      }
    }

    const writeState = new Uint32Array(2);
    const handle = new zlibUtil.ZlibStream(mode);

    handle.initialize(
      windowBits,
      level,
      memLevel,
      strategy,
      writeState,

      () => {
        // eslint-disable-next-line @typescript-eslint/no-unsafe-call
        queueMicrotask(processCallback.bind(handle));
      },
      dictionary
    );
    super(options ?? {}, mode, handle);
    handle[owner_symbol] = this;
    this._level = level;
    this._strategy = strategy;
    this._handle = handle;
    this._writeState = writeState;
  }

  public params(level: number, strategy: number, callback: () => void): void {
    checkRangesOrGetDefault(
      level,
      'level',
      CONST_Z_MIN_LEVEL,
      CONST_Z_MAX_LEVEL
    );
    checkRangesOrGetDefault(
      strategy,
      'strategy',
      CONST_Z_DEFAULT_STRATEGY,
      CONST_Z_FIXED
    );

    if (this._level !== level || this._strategy !== strategy) {
      this.flush(
        CONST_Z_SYNC_FLUSH,
        this.#paramsAfterFlushCallback.bind(this, level, strategy, callback)
      );
    } else {
      /* eslint-disable-next-line @typescript-eslint/no-unsafe-call */
      queueMicrotask(() => callback());
    }
  }

  // This callback is used by `.params()` to wait until a full flush happened
  // before adjusting the parameters. In particular, the call to the native
  // `params()` function should not happen while a write is currently in progress
  // on the threadpool.
  #paramsAfterFlushCallback(
    level: number,
    strategy: number,
    callback: () => void
  ): void {
    ok(this._handle, 'zlib binding closed');
    this._handle.params(level, strategy);
    if (!this.destroyed) {
      this._level = level;
      this._strategy = strategy;
      callback?.();
    }
  }
}
