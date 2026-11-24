// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { owner_symbol, type Zlib } from 'node-internal:internal_zlib_base';

type InternalCompressCallback = (res: Error | ArrayBuffer) => void;

export function crc32(data: ArrayBufferView | string, value: number): number;

export function zlibSync(
  data: ArrayBufferView | string,
  options: ZlibOptions,
  mode: number
): ArrayBuffer;
export function zlib(
  data: ArrayBufferView | string,
  options: ZlibOptions,
  mode: number,
  cb: InternalCompressCallback
): void;

export function brotliDecompressSync(
  data: ArrayBufferView | string,
  options: BrotliOptions
): ArrayBuffer;
export function brotliDecompress(
  data: ArrayBufferView | string,
  options: BrotliOptions,
  cb: InternalCompressCallback
): void;

export function brotliCompressSync(
  data: ArrayBufferView | string,
  options: BrotliOptions
): ArrayBuffer;
export function brotliCompress(
  data: ArrayBufferView | string,
  options: BrotliOptions,
  cb: InternalCompressCallback
): void;

export function zstdDecompressSync(
  data: ArrayBufferView | string,
  options: ZstdOptions
): ArrayBuffer;
export function zstdDecompress(
  data: ArrayBufferView | string,
  options: ZstdOptions,
  cb: InternalCompressCallback
): void;

export function zstdCompressSync(
  data: ArrayBufferView | string,
  options: ZstdOptions
): ArrayBuffer;
export function zstdCompress(
  data: ArrayBufferView | string,
  options: ZstdOptions,
  cb: InternalCompressCallback
): void;

// The keys and order of zlib.constants.
export type ZlibConstants = {
  readonly Z_NO_FLUSH: number;
  readonly Z_PARTIAL_FLUSH: number;
  readonly Z_SYNC_FLUSH: number;
  readonly Z_FULL_FLUSH: number;
  readonly Z_FINISH: number;
  readonly Z_BLOCK: number;
  readonly Z_OK: number;
  readonly Z_STREAM_END: number;
  readonly Z_NEED_DICT: number;
  readonly Z_ERRNO: number;
  readonly Z_STREAM_ERROR: number;
  readonly Z_DATA_ERROR: number;
  readonly Z_MEM_ERROR: number;
  readonly Z_BUF_ERROR: number;
  readonly Z_VERSION_ERROR: number;
  readonly Z_NO_COMPRESSION: number;
  readonly Z_BEST_SPEED: number;
  readonly Z_BEST_COMPRESSION: number;
  readonly Z_DEFAULT_COMPRESSION: number;
  readonly Z_FILTERED: number;
  readonly Z_HUFFMAN_ONLY: number;
  readonly Z_RLE: number;
  readonly Z_FIXED: number;
  readonly Z_DEFAULT_STRATEGY: number;
  readonly ZLIB_VERNUM: number;
  readonly DEFLATE: number;
  readonly INFLATE: number;
  readonly GZIP: number;
  readonly GUNZIP: number;
  readonly DEFLATERAW: number;
  readonly INFLATERAW: number;
  readonly UNZIP: number;
  readonly BROTLI_DECODE: number;
  readonly BROTLI_ENCODE: number;
  readonly Z_MIN_WINDOWBITS: number;
  readonly Z_MAX_WINDOWBITS: number;
  readonly Z_DEFAULT_WINDOWBITS: number;
  readonly Z_MIN_CHUNK: number;
  readonly Z_MAX_CHUNK: number;
  readonly Z_DEFAULT_CHUNK: number;
  readonly Z_MIN_MEMLEVEL: number;
  readonly Z_MAX_MEMLEVEL: number;
  readonly Z_DEFAULT_MEMLEVEL: number;
  readonly Z_MIN_LEVEL: number;
  readonly Z_MAX_LEVEL: number;
  readonly Z_DEFAULT_LEVEL: number;
  readonly BROTLI_OPERATION_PROCESS: number;
  readonly BROTLI_OPERATION_FLUSH: number;
  readonly BROTLI_OPERATION_FINISH: number;
  readonly BROTLI_OPERATION_EMIT_METADATA: number;
  readonly BROTLI_PARAM_MODE: number;
  readonly BROTLI_MODE_GENERIC: number;
  readonly BROTLI_MODE_TEXT: number;
  readonly BROTLI_MODE_FONT: number;
  readonly BROTLI_DEFAULT_MODE: number;
  readonly BROTLI_PARAM_QUALITY: number;
  readonly BROTLI_MIN_QUALITY: number;
  readonly BROTLI_MAX_QUALITY: number;
  readonly BROTLI_DEFAULT_QUALITY: number;
  readonly BROTLI_PARAM_LGWIN: number;
  readonly BROTLI_MIN_WINDOW_BITS: number;
  readonly BROTLI_MAX_WINDOW_BITS: number;
  readonly BROTLI_LARGE_MAX_WINDOW_BITS: number;
  readonly BROTLI_DEFAULT_WINDOW: number;
  readonly BROTLI_PARAM_LGBLOCK: number;
  readonly BROTLI_MIN_INPUT_BLOCK_BITS: number;
  readonly BROTLI_MAX_INPUT_BLOCK_BITS: number;
  readonly BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING: number;
  readonly BROTLI_PARAM_SIZE_HINT: number;
  readonly BROTLI_PARAM_LARGE_WINDOW: number;
  readonly BROTLI_PARAM_NPOSTFIX: number;
  readonly BROTLI_PARAM_NDIRECT: number;
  readonly BROTLI_DECODER_RESULT_ERROR: number;
  readonly BROTLI_DECODER_RESULT_SUCCESS: number;
  readonly BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: number;
  readonly BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: number;
  readonly BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION: number;
  readonly BROTLI_DECODER_PARAM_LARGE_WINDOW: number;
  readonly BROTLI_DECODER_NO_ERROR: number;
  readonly BROTLI_DECODER_SUCCESS: number;
  readonly BROTLI_DECODER_NEEDS_MORE_INPUT: number;
  readonly BROTLI_DECODER_NEEDS_MORE_OUTPUT: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_RESERVED: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_CL_SPACE: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_TRANSFORM: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_DICTIONARY: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_PADDING_1: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_PADDING_2: number;
  readonly BROTLI_DECODER_ERROR_FORMAT_DISTANCE: number;
  readonly BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET: number;
  readonly BROTLI_DECODER_ERROR_INVALID_ARGUMENTS: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2: number;
  readonly BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES: number;
  readonly BROTLI_DECODER_ERROR_UNREACHABLE: number;
  readonly ZSTD_ENCODE: number;
  readonly ZSTD_DECODE: number;
  readonly ZSTD_COMPRESS: number;
  readonly ZSTD_DECOMPRESS: number;
  readonly ZSTD_e_continue: number;
  readonly ZSTD_e_flush: number;
  readonly ZSTD_e_end: number;
  readonly ZSTD_c_compressionLevel: number;
  readonly ZSTD_c_windowLog: number;
  readonly ZSTD_c_hashLog: number;
  readonly ZSTD_c_chainLog: number;
  readonly ZSTD_c_searchLog: number;
  readonly ZSTD_c_minMatch: number;
  readonly ZSTD_c_targetLength: number;
  readonly ZSTD_c_strategy: number;
  readonly ZSTD_c_enableLongDistanceMatching: number;
  readonly ZSTD_c_ldmHashLog: number;
  readonly ZSTD_c_ldmMinMatch: number;
  readonly ZSTD_c_ldmBucketSizeLog: number;
  readonly ZSTD_c_ldmHashRateLog: number;
  readonly ZSTD_c_contentSizeFlag: number;
  readonly ZSTD_c_checksumFlag: number;
  readonly ZSTD_c_dictIDFlag: number;
  readonly ZSTD_c_nbWorkers: number;
  readonly ZSTD_c_jobSize: number;
  readonly ZSTD_c_overlapLog: number;
  readonly ZSTD_d_windowLogMax: number;
  readonly ZSTD_fast: number;
  readonly ZSTD_dfast: number;
  readonly ZSTD_greedy: number;
  readonly ZSTD_lazy: number;
  readonly ZSTD_lazy2: number;
  readonly ZSTD_btlazy2: number;
  readonly ZSTD_btopt: number;
  readonly ZSTD_btultra: number;
  readonly ZSTD_btultra2: number;
  readonly ZSTD_CLEVEL_DEFAULT: number;
  readonly ZSTD_error_no_error: number;
  readonly ZSTD_error_GENERIC: number;
  readonly ZSTD_error_prefix_unknown: number;
  readonly ZSTD_error_version_unsupported: number;
  readonly ZSTD_error_frameParameter_unsupported: number;
  readonly ZSTD_error_frameParameter_windowTooLarge: number;
  readonly ZSTD_error_corruption_detected: number;
  readonly ZSTD_error_checksum_wrong: number;
  readonly ZSTD_error_literals_headerWrong: number;
  readonly ZSTD_error_dictionary_corrupted: number;
  readonly ZSTD_error_dictionary_wrong: number;
  readonly ZSTD_error_dictionaryCreation_failed: number;
  readonly ZSTD_error_parameter_unsupported: number;
  readonly ZSTD_error_parameter_combination_unsupported: number;
  readonly ZSTD_error_parameter_outOfBound: number;
  readonly ZSTD_error_tableLog_tooLarge: number;
  readonly ZSTD_error_maxSymbolValue_tooLarge: number;
  readonly ZSTD_error_maxSymbolValue_tooSmall: number;
  readonly ZSTD_error_stabilityCondition_notRespected: number;
  readonly ZSTD_error_stage_wrong: number;
  readonly ZSTD_error_init_missing: number;
  readonly ZSTD_error_memory_allocation: number;
  readonly ZSTD_error_workSpace_tooSmall: number;
  readonly ZSTD_error_dstSize_tooSmall: number;
  readonly ZSTD_error_srcSize_wrong: number;
  readonly ZSTD_error_dstBuffer_null: number;
  readonly ZSTD_error_noForwardProgress_destFull: number;
  readonly ZSTD_error_noForwardProgress_inputEmpty: number;
};

export function getConstants(): ZlibConstants;

export interface ZlibOptions {
  flush?: number | undefined;
  finishFlush?: number | undefined;
  chunkSize?: number | undefined;
  windowBits?: number | undefined;
  level?: number | undefined; // compression only
  memLevel?: number | undefined; // compression only
  strategy?: number | undefined; // compression only
  dictionary?: ArrayBufferView | undefined; // deflate/inflate only, empty dictionary by default
  info?: boolean | undefined;
  maxOutputLength?: number | undefined;
}

export interface BrotliOptions {
  flush?: number | undefined;
  finishFlush?: number | undefined;
  chunkSize?: number | undefined;
  params?:
    | {
        [key: number]: boolean | number;
      }
    | undefined;
  maxOutputLength?: number | undefined;
  // Not specified in NodeJS docs but the tests expect it
  info?: boolean | undefined;
}

export interface ZstdOptions {
  flush?: number | undefined;
  finishFlush?: number | undefined;
  chunkSize?: number | undefined;
  params?:
    | {
        [key: number]: boolean | number;
      }
    | undefined;
  maxOutputLength?: number | undefined;
  pledgedSrcSize?: number | undefined;
  // Not specified in NodeJS docs but the tests expect it
  info?: boolean | undefined;
}

type ErrorHandler = (errno: number, code: string, message: string) => void;
type ProcessHandler = () => void;

export abstract class CompressionStream {
  [owner_symbol]: Zlib;
  // Not used by C++ implementation but required to be Node.js compatible.
  inOff: number;
  buffer: NodeJS.TypedArray | null;
  cb: () => void;
  availOutBefore: number;
  availInBefore: number;
  flushFlag: number;

  constructor(mode: number);
  close(): void;
  write(
    flushFlag: number,
    inputBuffer: NodeJS.TypedArray,
    inputOffset: number,
    inputLength: number,
    outputBuffer: NodeJS.TypedArray,
    outputOffset: number,
    outputLength: number
  ): void;
  writeSync(
    flushFlag: number,
    inputBuffer: NodeJS.TypedArray,
    inputOffset: number,
    inputLength: number,
    outputBuffer: NodeJS.TypedArray,
    outputOffset: number,
    outputLength: number
  ): void;
  reset(): void;

  // Workerd specific functions
  setErrorHandler(cb: ErrorHandler): void;
}

export class ZlibStream extends CompressionStream {
  initialize(
    windowBits: number,
    level: number,
    memLevel: number,
    strategy: number,
    writeState: NodeJS.TypedArray,
    processCallback: ProcessHandler,
    dictionary: ZlibOptions['dictionary']
  ): void;
  params(level: number, strategy: number): void;
}

export class BrotliDecoder extends CompressionStream {
  initialize(
    params: Uint32Array,
    writeResult: Uint32Array,
    writeCallback: () => void
  ): boolean;
  params(): void;
}

export class BrotliEncoder extends CompressionStream {
  initialize(
    params: Uint32Array,
    writeResult: Uint32Array,
    writeCallback: () => void
  ): boolean;
  params(): void;
}

export class ZstdDecoder extends CompressionStream {
  initialize(
    params: Int32Array,
    writeResult: Uint32Array,
    writeCallback: () => void,
    pledgedSrcSize?: number
  ): boolean;
  params(): void;
}

export class ZstdEncoder extends CompressionStream {
  initialize(
    params: Int32Array,
    writeResult: Uint32Array,
    writeCallback: () => void,
    pledgedSrcSize?: number
  ): boolean;
  params(): void;
}
