// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { default as zlibUtil, type ZlibOptions } from 'node-internal:zlib';
import { Buffer } from 'node-internal:internal_buffer';
import { validateUint32 } from 'node-internal:validators';
import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';
import { isArrayBufferView } from 'node-internal:internal_types';
import { Zlib } from 'node-internal:internal_zlib_base';

const {
  CONST_DEFLATE,
  CONST_DEFLATERAW,
  CONST_INFLATE,
  CONST_INFLATERAW,
  CONST_GUNZIP,
  CONST_GZIP,
  CONST_UNZIP,
} = zlibUtil;

const constPrefix = 'CONST_';
export const constants: Record<string, number> = {};

Object.defineProperties(
  constants,
  Object.fromEntries(
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument
    Object.entries(Object.getPrototypeOf(zlibUtil))
      .filter(([k]) => k.startsWith(constPrefix))
      .map(([k, v]) => [
        k.slice(constPrefix.length),
        {
          value: v,
          writable: false,
          configurable: false,
          enumerable: true,
        },
      ])
  )
);

export function crc32(
  data: ArrayBufferView | string,
  value: number = 0
): number {
  if (typeof data === 'string') {
    data = Buffer.from(data);
  } else if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE('data', 'ArrayBufferView', typeof data);
  }
  validateUint32(value, 'value');
  return zlibUtil.crc32(data, value);
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
