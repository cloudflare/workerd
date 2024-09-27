import * as zlib from 'node-internal:internal_zlib';
import { crc32 } from 'node-internal:internal_zlib';
import { constants, codes } from 'node-internal:internal_zlib_constants';
import { default as compatFlags } from 'workerd:compatibility-flags';

const { nodeJsZlib } = compatFlags;
const {
  Z_NO_FLUSH,
  Z_PARTIAL_FLUSH,
  Z_SYNC_FLUSH,
  Z_FULL_FLUSH,
  Z_FINISH,
  Z_BLOCK,

  Z_OK,
  Z_STREAM_END,
  Z_NEED_DICT,
  Z_ERRNO,
  Z_STREAM_ERROR,
  Z_DATA_ERROR,
  Z_MEM_ERROR,
  Z_BUF_ERROR,
  Z_VERSION_ERROR,

  Z_NO_COMPRESSION,
  Z_BEST_SPEED,
  Z_BEST_COMPRESSION,
  Z_DEFAULT_COMPRESSION,
  Z_FILTERED,
  Z_HUFFMAN_ONLY,
  Z_RLE,
  Z_FIXED,
  Z_DEFAULT_STRATEGY,
  ZLIB_VERNUM,

  DEFLATE,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP,
  BROTLI_DECODE,
  BROTLI_ENCODE,

  Z_MIN_WINDOWBITS,
  Z_MAX_WINDOWBITS,
  Z_DEFAULT_WINDOWBITS,
  Z_MIN_CHUNK,
  Z_MAX_CHUNK,
  Z_DEFAULT_CHUNK,
  Z_MIN_MEMLEVEL,
  Z_MAX_MEMLEVEL,
  Z_DEFAULT_MEMLEVEL,
  Z_MIN_LEVEL,
  Z_MAX_LEVEL,
  Z_DEFAULT_LEVEL,

  BROTLI_OPERATION_PROCESS,
  BROTLI_OPERATION_FLUSH,
  BROTLI_OPERATION_FINISH,
  BROTLI_OPERATION_EMIT_METADATA,
  BROTLI_PARAM_MODE,
  BROTLI_MODE_GENERIC,
  BROTLI_MODE_TEXT,
  BROTLI_MODE_FONT,
  BROTLI_DEFAULT_MODE,
  BROTLI_PARAM_QUALITY,
  BROTLI_MIN_QUALITY,
  BROTLI_MAX_QUALITY,
  BROTLI_DEFAULT_QUALITY,
  BROTLI_PARAM_LGWIN,
  BROTLI_MIN_WINDOW_BITS,
  BROTLI_MAX_WINDOW_BITS,
  BROTLI_LARGE_MAX_WINDOW_BITS,
  BROTLI_DEFAULT_WINDOW,
  BROTLI_PARAM_LGBLOCK,
  BROTLI_MIN_INPUT_BLOCK_BITS,
  BROTLI_MAX_INPUT_BLOCK_BITS,
  BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING,
  BROTLI_PARAM_LARGE_WINDOW,
  BROTLI_PARAM_NPOSTFIX,
  BROTLI_PARAM_NDIRECT,
  BROTLI_DECODER_RESULT_ERROR,
  BROTLI_DECODER_RESULT_SUCCESS,
  BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT,
  BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT,
  BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION,
  BROTLI_DECODER_PARAM_LARGE_WINDOW,
  BROTLI_DECODER_NO_ERROR,
  BROTLI_DECODER_SUCCESS,
  BROTLI_DECODER_NEEDS_MORE_INPUT,
  BROTLI_DECODER_NEEDS_MORE_OUTPUT,
  BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE,
  BROTLI_DECODER_ERROR_FORMAT_RESERVED,
  BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE,
  BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET,
  BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME,
  BROTLI_DECODER_ERROR_FORMAT_CL_SPACE,
  BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE,
  BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT,
  BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1,
  BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2,
  BROTLI_DECODER_ERROR_FORMAT_TRANSFORM,
  BROTLI_DECODER_ERROR_FORMAT_DICTIONARY,
  BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS,
  BROTLI_DECODER_ERROR_FORMAT_PADDING_1,
  BROTLI_DECODER_ERROR_FORMAT_PADDING_2,
  BROTLI_DECODER_ERROR_FORMAT_DISTANCE,
  BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET,
  BROTLI_DECODER_ERROR_INVALID_ARGUMENTS,
  BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES,
  BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS,
  BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP,
  BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1,
  BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2,
  BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES,
  BROTLI_DECODER_ERROR_UNREACHABLE,
} = constants;

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
const brotliCompress = protectMethod(zlib.brotliCompress);
const brotliCompressSync = protectMethod(zlib.brotliCompressSync);
const brotliDecompress = protectMethod(zlib.brotliDecompress);
const brotliDecompressSync = protectMethod(zlib.brotliDecompressSync);

export {
  crc32,
  codes,
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
  brotliDecompress,
  brotliDecompressSync,
  brotliCompress,
  brotliCompressSync,

  // NodeJS also exports all constants directly under zlib, but this is deprecated
  Z_NO_FLUSH,
  Z_PARTIAL_FLUSH,
  Z_SYNC_FLUSH,
  Z_FULL_FLUSH,
  Z_FINISH,
  Z_BLOCK,
  Z_OK,
  Z_STREAM_END,
  Z_NEED_DICT,
  Z_ERRNO,
  Z_STREAM_ERROR,
  Z_DATA_ERROR,
  Z_MEM_ERROR,
  Z_BUF_ERROR,
  Z_VERSION_ERROR,
  Z_NO_COMPRESSION,
  Z_BEST_SPEED,
  Z_BEST_COMPRESSION,
  Z_DEFAULT_COMPRESSION,
  Z_FILTERED,
  Z_HUFFMAN_ONLY,
  Z_RLE,
  Z_FIXED,
  Z_DEFAULT_STRATEGY,
  ZLIB_VERNUM,
  DEFLATE,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP,
  BROTLI_DECODE,
  BROTLI_ENCODE,
  Z_MIN_WINDOWBITS,
  Z_MAX_WINDOWBITS,
  Z_DEFAULT_WINDOWBITS,
  Z_MIN_CHUNK,
  Z_MAX_CHUNK,
  Z_DEFAULT_CHUNK,
  Z_MIN_MEMLEVEL,
  Z_MAX_MEMLEVEL,
  Z_DEFAULT_MEMLEVEL,
  Z_MIN_LEVEL,
  Z_MAX_LEVEL,
  Z_DEFAULT_LEVEL,
  BROTLI_OPERATION_PROCESS,
  BROTLI_OPERATION_FLUSH,
  BROTLI_OPERATION_FINISH,
  BROTLI_OPERATION_EMIT_METADATA,
  BROTLI_PARAM_MODE,
  BROTLI_MODE_GENERIC,
  BROTLI_MODE_TEXT,
  BROTLI_MODE_FONT,
  BROTLI_DEFAULT_MODE,
  BROTLI_PARAM_QUALITY,
  BROTLI_MIN_QUALITY,
  BROTLI_MAX_QUALITY,
  BROTLI_DEFAULT_QUALITY,
  BROTLI_PARAM_LGWIN,
  BROTLI_MIN_WINDOW_BITS,
  BROTLI_MAX_WINDOW_BITS,
  BROTLI_LARGE_MAX_WINDOW_BITS,
  BROTLI_DEFAULT_WINDOW,
  BROTLI_PARAM_LGBLOCK,
  BROTLI_MIN_INPUT_BLOCK_BITS,
  BROTLI_MAX_INPUT_BLOCK_BITS,
  BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING,
  BROTLI_PARAM_LARGE_WINDOW,
  BROTLI_PARAM_NPOSTFIX,
  BROTLI_PARAM_NDIRECT,
  BROTLI_DECODER_RESULT_ERROR,
  BROTLI_DECODER_RESULT_SUCCESS,
  BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT,
  BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT,
  BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION,
  BROTLI_DECODER_PARAM_LARGE_WINDOW,
  BROTLI_DECODER_NO_ERROR,
  BROTLI_DECODER_SUCCESS,
  BROTLI_DECODER_NEEDS_MORE_INPUT,
  BROTLI_DECODER_NEEDS_MORE_OUTPUT,
  BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE,
  BROTLI_DECODER_ERROR_FORMAT_RESERVED,
  BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE,
  BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET,
  BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME,
  BROTLI_DECODER_ERROR_FORMAT_CL_SPACE,
  BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE,
  BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT,
  BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1,
  BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2,
  BROTLI_DECODER_ERROR_FORMAT_TRANSFORM,
  BROTLI_DECODER_ERROR_FORMAT_DICTIONARY,
  BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS,
  BROTLI_DECODER_ERROR_FORMAT_PADDING_1,
  BROTLI_DECODER_ERROR_FORMAT_PADDING_2,
  BROTLI_DECODER_ERROR_FORMAT_DISTANCE,
  BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET,
  BROTLI_DECODER_ERROR_INVALID_ARGUMENTS,
  BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES,
  BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS,
  BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP,
  BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1,
  BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2,
  BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES,
  BROTLI_DECODER_ERROR_UNREACHABLE,
};

export default {
  crc32,
  codes,
  constants,
  // NodeJS also exports all constants directly under zlib, but this is deprecated
  ...constants,

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
  brotliDecompress,
  brotliDecompressSync,
  brotliCompress,
  brotliCompressSync,
};
