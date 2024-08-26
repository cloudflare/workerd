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
const createGzip = protectMethod(zlib.createGzip);
const createGunzip = protectMethod(zlib.createGunzip);
const createDeflate = protectMethod(zlib.createDeflate);
const createDeflateRaw = protectMethod(zlib.createDeflateRaw);
const createInflate = protectMethod(zlib.createInflate);
const createInflateRaw = protectMethod(zlib.createInflateRaw);
const createUnzip = protectMethod(zlib.createUnzip);

const inflate = protectMethod(zlib.inflate);
const inflateSync = protectMethod(zlib.inflateSync);
const deflate = protectMethod(zlib.deflate);
const deflateSync = protectMethod(zlib.deflateSync);

const inflateRaw = protectMethod(zlib.inflateRaw);
const inflateRawSync = protectMethod(zlib.inflateRawSync);
const deflateRaw = protectMethod(zlib.deflateRaw);
const deflateRawSync = protectMethod(zlib.deflateRawSync);

const gzip = protectMethod(zlib.gzip);
const gzipSync = protectMethod(zlib.gzipSync);
const gunzip = protectMethod(zlib.gunzip);
const gunzipSync = protectMethod(zlib.gunzipSync);

const unzip = protectMethod(zlib.unzip);
const unzipSync = protectMethod(zlib.unzipSync);

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

  // Convenience methods to create classes
  createGzip,
  createGunzip,
  createDeflate,
  createDeflateRaw,
  createInflate,
  createInflateRaw,
  createUnzip,

  // One-shot methods
  inflate,
  inflateSync,
  deflate,
  deflateSync,
  inflateRaw,
  inflateRawSync,
  deflateRaw,
  deflateRawSync,
  gzip,
  gzipSync,
  gunzip,
  gunzipSync,
  unzip,
  unzipSync,
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

  // Convenience methods to create classes
  createGzip,
  createGunzip,
  createDeflate,
  createDeflateRaw,
  createInflate,
  createInflateRaw,
  createUnzip,

  // One-shot methods
  inflate,
  inflateSync,
  deflate,
  deflateSync,
  inflateRaw,
  inflateRawSync,
  deflateRaw,
  deflateRawSync,
  gzip,
  gzipSync,
  gunzip,
  gunzipSync,
  unzip,
  unzipSync,
};
