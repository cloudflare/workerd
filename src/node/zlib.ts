import * as zlib from 'node-internal:internal_zlib';
import { crc32, constants } from 'node-internal:internal_zlib';
import { default as compatFlags } from 'workerd:compatibility-flags';

const { nodeJsZlib } = compatFlags;

function protectMethod(method: unknown): unknown {
  if (!nodeJsZlib) {
    return function notImplemented() {
      throw new Error('Compatibility flag "nodejs_zlib" is not enabled');
    };
  }

  return method;
}

const Gzip = protectMethod(zlib.Gzip);
const Gunzip = protectMethod(zlib.Gunzip);
const Deflate = protectMethod(zlib.Deflate);
const DeflateRaw = protectMethod(zlib.DeflateRaw);
const Inflate = protectMethod(zlib.Inflate);
const InflateRaw = protectMethod(zlib.InflateRaw);
const Unzip = protectMethod(zlib.Unzip);
const BrotliCompress = protectMethod(zlib.BrotliCompress);
const BrotliDecompress = protectMethod(zlib.BrotliDecompress);

const createGzip = protectMethod(zlib.createGzip);
const createGunzip = protectMethod(zlib.createGunzip);
const createDeflate = protectMethod(zlib.createDeflate);
const createDeflateRaw = protectMethod(zlib.createDeflateRaw);
const createInflate = protectMethod(zlib.createInflate);
const createInflateRaw = protectMethod(zlib.createInflateRaw);
const createUnzip = protectMethod(zlib.createUnzip);
const createBrotliCompress = protectMethod(zlib.createBrotliCompress);
const createBrotliDecompress = protectMethod(zlib.createBrotliDecompress);

export {
  crc32,
  constants,

  // Classes
  Gzip,
  Gunzip,
  Deflate,
  DeflateRaw,
  Inflate,
  InflateRaw,
  Unzip,
  BrotliCompress,
  BrotliDecompress,

  // Convenience methods to create classes
  createGzip,
  createGunzip,
  createDeflate,
  createDeflateRaw,
  createInflate,
  createInflateRaw,
  createUnzip,
  createBrotliCompress,
  createBrotliDecompress,
};

export default {
  crc32,
  constants,

  // Classes
  Gzip,
  Gunzip,
  Deflate,
  DeflateRaw,
  Inflate,
  InflateRaw,
  Unzip,
  BrotliCompress,
  BrotliDecompress,

  // Convenience methods to create classes
  createGzip,
  createGunzip,
  createDeflate,
  createDeflateRaw,
  createInflate,
  createInflateRaw,
  createUnzip,
  createBrotliCompress,
  createBrotliDecompress,
};
