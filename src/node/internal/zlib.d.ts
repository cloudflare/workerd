import { owner_symbol, type Zlib } from 'node-internal:internal_zlib_base';

export function crc32(data: ArrayBufferView, value: number): number;

export type CompressCallback<ErrT, BufT> = (
  error?: ErrT,
  result?: BufT
) => void;

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
  cb: CompressCallback<string, ArrayBuffer>
): ArrayBuffer;

// zlib.constants (part of the API contract for node:zlib)
export const CONST_Z_NO_FLUSH: number;
export const CONST_Z_PARTIAL_FLUSH: number;
export const CONST_Z_SYNC_FLUSH: number;
export const CONST_Z_FULL_FLUSH: number;
export const CONST_Z_FINISH: number;
export const CONST_Z_BLOCK: number;

export const CONST_Z_OK: number;
export const CONST_Z_STREAM_END: number;
export const CONST_Z_NEED_DICT: number;
export const CONST_Z_ERRNO: number;
export const CONST_Z_STREAM_ERROR: number;
export const CONST_Z_DATA_ERROR: number;
export const CONST_Z_MEM_ERROR: number;
export const CONST_Z_BUF_ERROR: number;
export const CONST_Z_VERSION_ERROR: number;

export const CONST_Z_NO_COMPRESSION: number;
export const CONST_Z_BEST_SPEED: number;
export const CONST_Z_BEST_COMPRESSION: number;
export const CONST_Z_DEFAULT_COMPRESSION: number;
export const CONST_Z_FILTERED: number;
export const CONST_Z_HUFFMAN_ONLY: number;
export const CONST_Z_RLE: number;
export const CONST_Z_FIXED: number;
export const CONST_Z_DEFAULT_STRATEGY: number;
export const CONST_ZLIB_VERNUM: number;

export const CONST_DEFLATE: number;
export const CONST_INFLATE: number;
export const CONST_GZIP: number;
export const CONST_GUNZIP: number;
export const CONST_DEFLATERAW: number;
export const CONST_INFLATERAW: number;
export const CONST_UNZIP: number;
export const CONST_BROTLI_DECODE: number;
export const CONST_BROTLI_ENCODE: number;

export const CONST_Z_MIN_WINDOWBITS: number;
export const CONST_Z_MAX_WINDOWBITS: number;
export const CONST_Z_DEFAULT_WINDOWBITS: number;
export const CONST_Z_MIN_CHUNK: number;
export const CONST_Z_MAX_CHUNK: number;
export const CONST_Z_DEFAULT_CHUNK: number;
export const CONST_Z_MIN_MEMLEVEL: number;
export const CONST_Z_MAX_MEMLEVEL: number;
export const CONST_Z_DEFAULT_MEMLEVEL: number;
export const CONST_Z_MIN_LEVEL: number;
export const CONST_Z_MAX_LEVEL: number;
export const CONST_Z_DEFAULT_LEVEL: number;

export const CONST_BROTLI_OPERATION_PROCESS: number;
export const CONST_BROTLI_OPERATION_FLUSH: number;
export const CONST_BROTLI_OPERATION_FINISH: number;
export const CONST_BROTLI_OPERATION_EMIT_METADATA: number;
export const CONST_BROTLI_PARAM_MODE: number;
export const CONST_BROTLI_MODE_GENERIC: number;
export const CONST_BROTLI_MODE_TEXT: number;
export const CONST_BROTLI_MODE_FONT: number;
export const CONST_BROTLI_DEFAULT_MODE: number;
export const CONST_BROTLI_PARAM_QUALITY: number;
export const CONST_BROTLI_MIN_QUALITY: number;
export const CONST_BROTLI_MAX_QUALITY: number;
export const CONST_BROTLI_DEFAULT_QUALITY: number;
export const CONST_BROTLI_PARAM_LGWIN: number;
export const CONST_BROTLI_MIN_WINDOW_BITS: number;
export const CONST_BROTLI_MAX_WINDOW_BITS: number;
export const CONST_BROTLI_LARGE_MAX_WINDOW_BITS: number;
export const CONST_BROTLI_DEFAULT_WINDOW: number;
export const CONST_BROTLI_PARAM_LGBLOCK: number;
export const CONST_BROTLI_MIN_INPUT_BLOCK_BITS: number;
export const CONST_BROTLI_MAX_INPUT_BLOCK_BITS: number;
export const CONST_BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING: number;
export const CONST_BROTLI_PARAM_SIZE_HINT: number;
export const CONST_BROTLI_PARAM_LARGE_WINDOW: number;
export const CONST_BROTLI_PARAM_NPOSTFIX: number;
export const CONST_BROTLI_PARAM_NDIRECT: number;
export const CONST_BROTLI_DECODER_RESULT_ERROR: number;
export const CONST_BROTLI_DECODER_RESULT_SUCCESS: number;
export const CONST_BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: number;
export const CONST_BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: number;
export const CONST_BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION: number;
export const CONST_BROTLI_DECODER_PARAM_LARGE_WINDOW: number;
export const CONST_BROTLI_DECODER_NO_ERROR: number;
export const CONST_BROTLI_DECODER_SUCCESS: number;
export const CONST_BROTLI_DECODER_NEEDS_MORE_INPUT: number;
export const CONST_BROTLI_DECODER_NEEDS_MORE_OUTPUT: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_RESERVED: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_CL_SPACE: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_TRANSFORM: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_DICTIONARY: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_PADDING_1: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_PADDING_2: number;
export const CONST_BROTLI_DECODER_ERROR_FORMAT_DISTANCE: number;
export const CONST_BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET: number;
export const CONST_BROTLI_DECODER_ERROR_INVALID_ARGUMENTS: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2: number;
export const CONST_BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES: number;
export const CONST_BROTLI_DECODER_ERROR_UNREACHABLE: number;

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
}

type ErrorHandler = (errno: number, code: string, message: string) => void;
type ProcessHandler = () => void;

export abstract class CompressionStream {
  public [owner_symbol]: Zlib;
  // Not used by C++ implementation but required to be Node.js compatible.
  public inOff: number;
  /* eslint-disable-next-line @typescript-eslint/no-redundant-type-constituents */
  public buffer: NodeJS.TypedArray | null;
  public cb: () => void;
  public availOutBefore: number;
  public availInBefore: number;
  public flushFlag: number;

  public constructor(mode: number);
  public close(): void;
  public write(
    flushFlag: number,
    inputBuffer: NodeJS.TypedArray,
    inputOffset: number,
    inputLength: number,
    outputBuffer: NodeJS.TypedArray,
    outputOffset: number,
    outputLength: number
  ): void;
  public writeSync(
    flushFlag: number,
    inputBuffer: NodeJS.TypedArray,
    inputOffset: number,
    inputLength: number,
    outputBuffer: NodeJS.TypedArray,
    outputOffset: number,
    outputLength: number
  ): void;
  public reset(): void;

  // Workerd specific functions
  public setErrorHandler(cb: ErrorHandler): void;
}

export class ZlibStream extends CompressionStream {
  public initialize(
    windowBits: number,
    level: number,
    memLevel: number,
    strategy: number,
    writeState: NodeJS.TypedArray,
    processCallback: ProcessHandler,
    dictionary: ZlibOptions['dictionary']
  ): void;
  public params(level: number, strategy: number): void;
}

export class BrotliDecoder extends CompressionStream {
  public initialize(
    params: Uint32Array,
    writeResult: Uint32Array,
    writeCallback: () => void
  ): boolean;
  public params(): void;
}

export class BrotliEncoder extends CompressionStream {
  public initialize(
    params: Uint32Array,
    writeResult: Uint32Array,
    writeCallback: () => void
  ): boolean;
  public params(): void;
}
