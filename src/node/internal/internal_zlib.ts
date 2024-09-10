// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  default as zlibUtil,
  type ZlibOptions,
  type BrotliOptions,
} from 'node-internal:zlib';
import { Buffer } from 'node-internal:internal_buffer';
import { validateUint32 } from 'node-internal:validators';
import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';
import { Zlib, Brotli, type ZlibBase } from 'node-internal:internal_zlib_base';

type ZlibResult = Buffer | { buffer: Buffer; engine: ZlibBase };
type CompressCallback = (err: Error | null, result?: ZlibResult) => void;

const {
  CONST_DEFLATE,
  CONST_DEFLATERAW,
  CONST_INFLATE,
  CONST_INFLATERAW,
  CONST_GUNZIP,
  CONST_GZIP,
  CONST_UNZIP,
  CONST_BROTLI_DECODE,
  CONST_BROTLI_ENCODE,
} = zlibUtil;

export function crc32(
  data: ArrayBufferView | string,
  value: number = 0
): number {
  validateUint32(value, 'value');
  return zlibUtil.crc32(data, value);
}

function processChunk(
  engine: ZlibBase,
  data: ArrayBufferView | string
): ZlibResult {
  return {
    engine,
    // TODO(soon): What is the proper way to deal with ArrayBufferView to Buffer typing issues?
    buffer: engine._processChunk(
      typeof data === 'string' ? Buffer.from(data) : (data as Buffer),
      engine._finishFlushFlag
    ),
  };
}

function zlibSyncImpl(
  data: ArrayBufferView | string,
  options: ZlibOptions,
  mode: ZlibMode
): ZlibResult {
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.zlibSync(data, options, mode));
  }

  // Else, use the Engine class in sync mode
  return processChunk(new CLASS_BY_MODE[mode](options), data);
}

export function inflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_INFLATE);
}

export function deflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_DEFLATE);
}

export function gunzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_GUNZIP);
}

export function gzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_GZIP);
}

export function inflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_INFLATERAW);
}

export function deflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_DEFLATERAW);
}

export function unzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, CONST_UNZIP);
}

export function brotliDecompressSync(
  data: ArrayBufferView | string,
  options: BrotliOptions = {}
): ZlibResult {
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.brotliDecompressSync(data, options));
  }

  // Else, use the Engine class in sync mode
  return processChunk(new BrotliDecompress(options), data);
}

export function brotliCompressSync(
  data: ArrayBufferView | string,
  options: BrotliOptions = {}
): ZlibResult {
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.brotliCompressSync(data, options));
  }

  // Else, use the Engine class in sync mode
  return processChunk(new BrotliCompress(options), data);
}

function normalizeArgs(
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): [ZlibOptions, CompressCallback] {
  if (typeof options === 'function') {
    return [{}, options];
  } else if (typeof callback === 'function') {
    return [options, callback];
  }

  throw new ERR_INVALID_ARG_TYPE('callback', 'Function', callback);
}

function processChunkCaptureError(
  engine: ZlibBase,
  data: ArrayBufferView | string,
  cb: CompressCallback
): void {
  try {
    const res = processChunk(engine, data);
    queueMicrotask(() => {
      cb(null, res);
    });
  } catch (err: unknown) {
    if (err instanceof Error) {
      queueMicrotask(() => {
        cb(err);
      });
      return;
    }

    const unreachable = new Error('Unreachable');
    unreachable.cause = err;
    throw unreachable;
  }
}

function zlibImpl(
  mode: ZlibMode,
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );

  if (!options.info) {
    // Fast path
    zlibUtil.zlib(data, options, mode, (res) => {
      queueMicrotask(() => {
        if (res instanceof Error) {
          callback(res);
        } else {
          callback(null, Buffer.from(res));
        }
      });
    });

    return;
  }

  processChunkCaptureError(new CLASS_BY_MODE[mode](options), data, callback);
}

export function inflate(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_INFLATE, data, options, callback);
}

export function unzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_UNZIP, data, options, callback);
}

export function inflateRaw(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_INFLATERAW, data, options, callback);
}

export function gunzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_GUNZIP, data, options, callback);
}

export function deflate(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_DEFLATE, data, options, callback);
}

export function deflateRaw(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_DEFLATERAW, data, options, callback);
}

export function gzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(CONST_GZIP, data, options, callback);
}

export function brotliDecompress(
  data: ArrayBufferView | string,
  optionsOrCallback: BrotliOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );

  if (!options.info) {
    // Fast path
    zlibUtil.brotliDecompress(data, options, (res) => {
      queueMicrotask(() => {
        if (res instanceof Error) {
          callback(res);
        } else {
          callback(null, Buffer.from(res));
        }
      });
    });

    return;
  }

  processChunkCaptureError(new BrotliDecompress(options), data, callback);
}

export function brotliCompress(
  data: ArrayBufferView | string,
  optionsOrCallback: BrotliOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );

  if (!options.info) {
    // Fast path
    zlibUtil.brotliCompress(data, options, (res) => {
      queueMicrotask(() => {
        if (res instanceof Error) {
          callback(res);
        } else {
          callback(null, Buffer.from(res));
        }
      });
    });

    return;
  }

  processChunkCaptureError(new BrotliCompress(options), data, callback);
}
export class Gzip extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_GZIP);
  }
}

export class Gunzip extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_GUNZIP);
  }
}

export class Deflate extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_DEFLATE);
  }
}

export class DeflateRaw extends Zlib {
  public constructor(options?: ZlibOptions) {
    if (options?.windowBits === 8) {
      options.windowBits = 9;
    }
    super(options, CONST_DEFLATERAW);
  }
}

export class Inflate extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_INFLATE);
  }
}

export class InflateRaw extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_INFLATERAW);
  }
}

export class Unzip extends Zlib {
  public constructor(options: ZlibOptions) {
    super(options, CONST_UNZIP);
  }
}

export class BrotliCompress extends Brotli {
  public constructor(options: BrotliOptions) {
    super(options, CONST_BROTLI_ENCODE);
  }
}

export class BrotliDecompress extends Brotli {
  public constructor(options: BrotliOptions) {
    super(options, CONST_BROTLI_DECODE);
  }
}

const CLASS_BY_MODE = {
  [ZlibMode.DEFLATE]: Deflate,
  [ZlibMode.INFLATE]: Inflate,
  [ZlibMode.DEFLATERAW]: DeflateRaw,
  [ZlibMode.INFLATERAW]: InflateRaw,
  [ZlibMode.GZIP]: Gzip,
  [ZlibMode.GUNZIP]: Gunzip,
  [ZlibMode.UNZIP]: Unzip,
};

export function createGzip(options: ZlibOptions): Gzip {
  return new Gzip(options);
}

export function createGunzip(options: ZlibOptions): Gunzip {
  return new Gunzip(options);
}

export function createDeflate(options: ZlibOptions): Deflate {
  return new Deflate(options);
}

export function createDeflateRaw(options: ZlibOptions): DeflateRaw {
  return new DeflateRaw(options);
}

export function createInflate(options: ZlibOptions): Inflate {
  return new Inflate(options);
}

export function createInflateRaw(options: ZlibOptions): InflateRaw {
  return new InflateRaw(options);
}

export function createUnzip(options: ZlibOptions): Unzip {
  return new Unzip(options);
}

export function createBrotliCompress(options: BrotliOptions): BrotliCompress {
  return new BrotliCompress(options);
}

export function createBrotliDecompress(
  options: BrotliOptions
): BrotliDecompress {
  return new BrotliDecompress(options);
}

const enum ZlibMode {
  DEFLATE = 1,
  INFLATE = 2,
  GZIP = 3,
  GUNZIP = 4,
  DEFLATERAW = 5,
  INFLATERAW = 6,
  UNZIP = 7,
}
