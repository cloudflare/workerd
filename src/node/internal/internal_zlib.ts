// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  default as zlibUtil,
  type ZlibOptions,
  type BrotliOptions,
  type ZstdOptions,
} from 'node-internal:zlib';
import { Buffer } from 'node-internal:internal_buffer';
import { constants } from 'node-internal:internal_zlib_constants';
import { validateUint32 } from 'node-internal:validators';
import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';
import { isAnyArrayBuffer } from 'node-internal:internal_types';
import {
  Zlib,
  Brotli,
  Zstd,
  zstdInitCParamsArray,
  zstdInitDParamsArray,
  kMaxZstdCParam,
  kMaxZstdDParam,
  type ZlibBase,
} from 'node-internal:internal_zlib_base';

type ZlibResult = Buffer | { buffer: Buffer; engine: ZlibBase };
type CompressCallback = (err: Error | null, result?: ZlibResult) => void;

const {
  DEFLATE,
  DEFLATERAW,
  INFLATE,
  INFLATERAW,
  GUNZIP,
  GZIP,
  UNZIP,
  BROTLI_DECODE,
  BROTLI_ENCODE,
  ZSTD_ENCODE,
  ZSTD_DECODE,
} = constants;

const ZlibMode = {
  DEFLATE: 1,
  INFLATE: 2,
  GZIP: 3,
  GUNZIP: 4,
  DEFLATERAW: 5,
  INFLATERAW: 6,
  UNZIP: 7,
};

export function crc32(
  data: ArrayBufferView | string,
  value: number = 0
): number {
  validateUint32(value, 'value');
  return zlibUtil.crc32(data, value);
}

function validateInput(data: unknown): void {
  if (
    typeof data !== 'string' &&
    !ArrayBuffer.isView(data) &&
    !isAnyArrayBuffer(data)
  ) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['string', 'Buffer', 'TypedArray', 'DataView', 'ArrayBuffer'],
      data
    );
  }
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
  mode: (typeof ZlibMode)[keyof typeof ZlibMode]
): ZlibResult {
  validateInput(data);
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.zlibSync(data, options, mode));
  }

  // Else, use the Engine class in sync mode
  // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
  return processChunk(new CLASS_BY_MODE[mode]!(options), data);
}

export function inflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, INFLATE);
}

export function deflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, DEFLATE);
}

export function gunzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, GUNZIP);
}

export function gzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, GZIP);
}

export function inflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, INFLATERAW);
}

export function deflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, DEFLATERAW);
}

export function unzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): ZlibResult {
  return zlibSyncImpl(data, options, UNZIP);
}

export function brotliDecompressSync(
  data: ArrayBufferView | string,
  options: BrotliOptions = {}
): ZlibResult {
  validateInput(data);
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
  validateInput(data);
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
  mode: (typeof ZlibMode)[keyof typeof ZlibMode],
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  validateInput(data);

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

  // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
  processChunkCaptureError(new CLASS_BY_MODE[mode]!(options), data, callback);
}

export function inflate(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(INFLATE, data, options, callback);
}

export function unzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(UNZIP, data, options, callback);
}

export function inflateRaw(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(INFLATERAW, data, options, callback);
}

export function gunzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(GUNZIP, data, options, callback);
}

export function deflate(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(DEFLATE, data, options, callback);
}

export function deflateRaw(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(DEFLATERAW, data, options, callback);
}

export function gzip(
  data: ArrayBufferView | string,
  options: ZlibOptions | CompressCallback,
  callback?: CompressCallback
): void {
  zlibImpl(GZIP, data, options, callback);
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
  validateInput(data);

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
  validateInput(data);

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

export function zstdDecompressSync(
  data: ArrayBufferView | string,
  options: ZstdOptions = {}
): ZlibResult {
  validateInput(data);
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.zstdDecompressSync(data, options));
  }

  // Else, use the Engine class in sync mode
  return processChunk(new ZstdDecompress(options), data);
}

export function zstdCompressSync(
  data: ArrayBufferView | string,
  options: ZstdOptions = {}
): ZlibResult {
  validateInput(data);
  if (!options.info) {
    // Fast path, where we send the data directly to C++
    return Buffer.from(zlibUtil.zstdCompressSync(data, options));
  }

  // Else, use the Engine class in sync mode
  return processChunk(new ZstdCompress(options), data);
}

export function zstdDecompress(
  data: ArrayBufferView | string,
  optionsOrCallback: ZstdOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  validateInput(data);

  if (!options.info) {
    // Fast path
    zlibUtil.zstdDecompress(data, options, (res) => {
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

  processChunkCaptureError(new ZstdDecompress(options), data, callback);
}

export function zstdCompress(
  data: ArrayBufferView | string,
  optionsOrCallback: ZstdOptions | CompressCallback,
  callbackOrUndefined?: CompressCallback
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  validateInput(data);

  if (!options.info) {
    // Fast path
    zlibUtil.zstdCompress(data, options, (res) => {
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

  processChunkCaptureError(new ZstdCompress(options), data, callback);
}
export class Gzip extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, GZIP);
  }
}

export class Gunzip extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, GUNZIP);
  }
}

export class Deflate extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, DEFLATE);
  }
}

export class DeflateRaw extends Zlib {
  constructor(options?: ZlibOptions) {
    if (options?.windowBits === 8) {
      options.windowBits = 9;
    }
    super(options, DEFLATERAW);
  }
}

export class Inflate extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, INFLATE);
  }
}

export class InflateRaw extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, INFLATERAW);
  }
}

export class Unzip extends Zlib {
  constructor(options: ZlibOptions) {
    super(options, UNZIP);
  }
}

export class BrotliCompress extends Brotli {
  constructor(options: BrotliOptions) {
    super(options, BROTLI_ENCODE);
  }
}

export class BrotliDecompress extends Brotli {
  constructor(options: BrotliOptions) {
    super(options, BROTLI_DECODE);
  }
}

export class ZstdCompress extends Zstd {
  constructor(options: ZstdOptions) {
    super(options, ZSTD_ENCODE, zstdInitCParamsArray, kMaxZstdCParam);
  }
}

export class ZstdDecompress extends Zstd {
  constructor(options: ZstdOptions) {
    super(options, ZSTD_DECODE, zstdInitDParamsArray, kMaxZstdDParam);
  }
}

const CLASS_BY_MODE: Record<
  (typeof ZlibMode)[keyof typeof ZlibMode],
  | typeof Deflate
  | typeof Inflate
  | typeof InflateRaw
  | typeof Unzip
  | typeof BrotliCompress
  | typeof BrotliDecompress
> = {
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

export function createZstdCompress(options: ZstdOptions): ZstdCompress {
  return new ZstdCompress(options);
}

export function createZstdDecompress(options: ZstdOptions): ZstdDecompress {
  return new ZstdDecompress(options);
}
