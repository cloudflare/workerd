// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  default as zlibUtil,
  type ZlibOptions,
  type CompressCallback,
  type BrotliOptions,
} from 'node-internal:zlib';
import { Buffer } from 'node-internal:internal_buffer';
import { validateUint32 } from 'node-internal:validators';
import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';
import { Zlib, Brotli } from 'node-internal:internal_zlib_base';

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

export function inflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(zlibUtil.zlibSync(data, options, zlibUtil.CONST_INFLATE));
}

export function deflateSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(zlibUtil.zlibSync(data, options, zlibUtil.CONST_DEFLATE));
}

export function gunzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(zlibUtil.zlibSync(data, options, zlibUtil.CONST_GUNZIP));
}

export function gzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(zlibUtil.zlibSync(data, options, zlibUtil.CONST_GZIP));
}

export function inflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(
    zlibUtil.zlibSync(data, options, zlibUtil.CONST_INFLATERAW)
  );
}

export function deflateRawSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(
    zlibUtil.zlibSync(data, options, zlibUtil.CONST_DEFLATERAW)
  );
}

export function unzipSync(
  data: ArrayBufferView | string,
  options: ZlibOptions = {}
): Buffer {
  return Buffer.from(zlibUtil.zlibSync(data, options, zlibUtil.CONST_UNZIP));
}

function normalizeArgs(
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): [ZlibOptions, CompressCallback<Error, Buffer>] {
  if (typeof optionsOrCallback === 'function') {
    return [{}, optionsOrCallback];
  } else if (typeof callbackOrUndefined === 'function') {
    return [optionsOrCallback, callbackOrUndefined];
  }

  throw new ERR_INVALID_ARG_TYPE('callback', 'Function', callbackOrUndefined);
}

function wrapCallback(
  callback: CompressCallback<Error, Buffer>
): CompressCallback<string, ArrayBuffer> {
  return (error: string | null, result: ArrayBuffer | undefined) => {
    queueMicrotask(() => {
      if (error) {
        callback(new Error(error));
      } else {
        // To avoid having a runtime assertion, let's use type assertion.
        callback(null, Buffer.from(result as ArrayBuffer));
      }
    });
  };
}

export function inflate(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(data, options, zlibUtil.CONST_INFLATE, wrapCallback(callback));
}

export function unzip(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(data, options, zlibUtil.CONST_UNZIP, wrapCallback(callback));
}

export function inflateRaw(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(
    data,
    options,
    zlibUtil.CONST_INFLATERAW,
    wrapCallback(callback)
  );
}

export function gunzip(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(data, options, zlibUtil.CONST_GUNZIP, wrapCallback(callback));
}

export function deflate(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(data, options, zlibUtil.CONST_DEFLATE, wrapCallback(callback));
}

export function deflateRaw(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(
    data,
    options,
    zlibUtil.CONST_DEFLATERAW,
    wrapCallback(callback)
  );
}

export function gzip(
  data: ArrayBufferView | string,
  optionsOrCallback: ZlibOptions | CompressCallback<Error, Buffer>,
  callbackOrUndefined?: CompressCallback<Error, Buffer>
): void {
  const [options, callback] = normalizeArgs(
    optionsOrCallback,
    callbackOrUndefined
  );
  zlibUtil.zlib(data, options, zlibUtil.CONST_GZIP, wrapCallback(callback));
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
