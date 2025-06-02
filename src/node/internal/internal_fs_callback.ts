import * as fssync from 'node-internal:internal_fs_sync';
import type {
  FStatOptions,
  MkdirTempSyncOptions,
  ReadDirOptions,
  ReadDirResult,
  ReadFileSyncOptions,
  ReadLinkSyncOptions,
  StatOptions,
  WriteSyncOptions,
} from 'node-internal:internal_fs_sync';
import {
  validatePosition,
  type FilePath,
} from 'node-internal:internal_fs_utils';
import { F_OK } from 'node-internal:internal_fs_constants';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';
import { Stats } from 'node-internal:internal_fs_utils';
import { type Dir } from 'node-internal:internal_fs';
import { Buffer } from 'node-internal:internal_buffer';
import { isArrayBufferView } from 'node-internal:internal_types';
import { validateUint32 } from 'node-internal:validators';
import type {
  BigIntStatsFs,
  CopySyncOptions,
  MakeDirectoryOptions,
  OpenDirOptions,
  ReadAsyncOptions,
  RmOptions,
  RmDirOptions,
  StatsFs,
  WriteFileOptions,
} from 'node:fs';

type ErrorOnlyCallback = (err: unknown) => void;
type SingleArgCallback<T> = (err: unknown, result?: T) => void;
type DoubleArgCallback<T, U> = (err: unknown, result1?: T, result2?: U) => void;

function callWithErrorOnlyCallback(
  fn: () => void,
  callback: undefined | ErrorOnlyCallback
): void {
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  try {
    fn();
    // Note that any errors thrown by the callback will be "handled" by passing
    // them along to the reportError function, which logs them and triggers the
    // global "error" event.
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(null));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(err));
  }
}

function callWithSingleArgCallback<T>(
  fn: () => T,
  callback: undefined | SingleArgCallback<T>
): void {
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  try {
    const result = fn();
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(null, result));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(err));
  }
}

export function access(path: FilePath, callback: ErrorOnlyCallback): void;
export function access(
  path: FilePath,
  mode: number,
  callback: ErrorOnlyCallback
): void;
export function access(
  path: FilePath,
  modeOrCallback: number | ErrorOnlyCallback = F_OK,
  callback?: ErrorOnlyCallback
): void {
  let mode: number;
  if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    mode = F_OK;
  } else {
    mode = modeOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.accessSync(path, mode), callback);
}

export type ExistsCallback = (result: boolean) => void;

export function exists(path: FilePath, callback: ExistsCallback): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  queueMicrotask(() => callback(fssync.existsSync(path)));
}

export function appendFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  callback: ErrorOnlyCallback
): void;
export function appendFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions,
  callback: ErrorOnlyCallback
): void;
export function appendFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  optionsOrCallback: WriteFileOptions | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let options: WriteFileOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      mode: 0o666,
      flag: 'a',
      flush: false,
    };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback<number>(
    () => fssync.appendFileSync(path, data, options),
    callback
  );
}

export function chmod(
  path: FilePath,
  mode: number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.chmodSync(path, mode), callback);
}

export function chown(
  path: FilePath,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.chownSync(path, uid, gid), callback);
}

export function close(
  fd: number,
  callback: ErrorOnlyCallback = () => {}
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.closeSync(fd), callback);
}

export function copyFile(
  src: FilePath,
  dest: FilePath,
  callback: ErrorOnlyCallback
): void;
export function copyFile(
  src: FilePath,
  dest: FilePath,
  mode: number,
  callback: ErrorOnlyCallback
): void;
export function copyFile(
  src: FilePath,
  dest: FilePath,
  modeOrCallback: number | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let mode: number;
  if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    mode = 0;
  } else {
    mode = modeOrCallback;
  }
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.copyFileSync(src, dest, mode),
    callback
  );
}

export function cp(
  src: FilePath,
  dest: FilePath,
  callback: ErrorOnlyCallback
): void;
export function cp(
  src: FilePath,
  dest: FilePath,
  options: CopySyncOptions,
  callback: ErrorOnlyCallback
): void;
export function cp(
  src: FilePath,
  dest: FilePath,
  optionsOrCallback: CopySyncOptions | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let options: CopySyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.cpSync(src, dest, options), callback);
}

export function fchmod(
  fd: number,
  mode: string | number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.fchmodSync(fd, mode), callback);
}

export function fchown(
  fd: number,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.fchownSync(fd, uid, gid), callback);
}

export function fdatasync(fd: number, callback: ErrorOnlyCallback): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.fdatasyncSync(fd), callback);
}

export function fstat(fd: number, callback: SingleArgCallback<Stats>): void;
export function fstat(
  fd: number,
  options: FStatOptions,
  callback: SingleArgCallback<Stats>
): void;
export function fstat(
  fd: number,
  optionsOrCallback: SingleArgCallback<Stats> | FStatOptions,
  callback?: SingleArgCallback<Stats>
): void {
  let options: FStatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.fstatSync(fd, options), callback);
}

export function fsync(
  fd: number,
  callback: ErrorOnlyCallback = () => {}
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.fsyncSync(fd), callback);
}

export function ftruncate(fd: number, callback: ErrorOnlyCallback): void;
export function ftruncate(
  fd: number,
  len: number,
  callback: ErrorOnlyCallback
): void;
export function ftruncate(
  fd: number,
  lenOrCallback: number | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let len: number;
  if (typeof lenOrCallback === 'function') {
    callback = lenOrCallback;
    len = 0;
  } else {
    len = lenOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.ftruncateSync(fd, len), callback);
}

export function futimes(
  fd: number,
  atime: string | number | Date,
  mtime: string | number | Date,
  callback: ErrorOnlyCallback
): void {
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.futimesSync(fd, atime, mtime),
    callback
  );
}

export function lchmod(
  path: FilePath,
  mode: string | number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.lchmodSync(path, mode), callback);
}

export function lchown(
  path: FilePath,
  uid: number,
  gid: number,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.lchownSync(path, uid, gid), callback);
}

export function lutimes(
  path: FilePath,
  atime: string | number | Date,
  mtime: string | number | Date,
  callback: ErrorOnlyCallback
): void {
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.lutimesSync(path, atime, mtime),
    callback
  );
}

export function link(
  src: FilePath,
  dest: FilePath,
  callback: ErrorOnlyCallback
): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.linkSync(src, dest), callback);
}

export function lstat(
  path: FilePath,
  callback: SingleArgCallback<Stats | undefined>
): void;
export function lstat(
  path: FilePath,
  options: StatOptions,
  callback: SingleArgCallback<Stats | undefined>
): void;
export function lstat(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Stats | undefined> | StatOptions,
  callback?: SingleArgCallback<Stats | undefined>
): void {
  let options: StatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.lstatSync(path, options), callback);
}

export function mkdir(
  path: FilePath,
  callback: SingleArgCallback<string | undefined>
): void;
export function mkdir(
  path: FilePath,
  options: number | MakeDirectoryOptions,
  callback: SingleArgCallback<string | undefined>
): void;
export function mkdir(
  path: FilePath,
  optionsOrCallback:
    | number
    | SingleArgCallback<string | undefined>
    | MakeDirectoryOptions,
  callback?: SingleArgCallback<string | undefined>
): void {
  let options: number | MakeDirectoryOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.mkdirSync(path, options), callback);
}

export function mkdtemp(
  prefix: FilePath,
  callback: SingleArgCallback<string>
): void;
export function mkdtemp(
  prefix: FilePath,
  options: MkdirTempSyncOptions,
  callback: SingleArgCallback<string>
): void;
export function mkdtemp(
  prefix: FilePath,
  optionsOrCallback: SingleArgCallback<string> | MkdirTempSyncOptions,
  callback?: SingleArgCallback<string>
): void {
  let options: MkdirTempSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(
    () => fssync.mkdtempSync(prefix, options),
    callback
  );
}

export function open(path: FilePath, callback: SingleArgCallback<number>): void;
export function open(
  path: FilePath,
  flags: string | number,
  callback: SingleArgCallback<number>
): void;
export function open(
  path: FilePath,
  flags: string | number,
  mode: string | number,
  callback: SingleArgCallback<number>
): void;
export function open(
  path: FilePath,
  flagsOrCallback: string | number | SingleArgCallback<number> = 'r',
  modeOrCallback: string | number | SingleArgCallback<number> = 0o666,
  callback?: SingleArgCallback<number>
): void {
  let flags: string | number;
  let mode: string | number;
  if (typeof flagsOrCallback === 'function') {
    callback = flagsOrCallback;
    flags = 'r';
    mode = 0o666;
  } else if (typeof modeOrCallback === 'function') {
    callback = modeOrCallback;
    mode = 0o666;
  } else {
    flags = flagsOrCallback;
    mode = modeOrCallback;
  }
  callWithSingleArgCallback(() => fssync.openSync(path, flags, mode), callback);
}

export function opendir(path: FilePath, callback: SingleArgCallback<Dir>): void;
export function opendir(
  path: FilePath,
  options: OpenDirOptions,
  callback: SingleArgCallback<Dir>
): void;
export function opendir(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Dir> | OpenDirOptions,
  callback?: SingleArgCallback<Dir>
): void {
  let options: OpenDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      bufferSize: 32,
      recursive: false,
    };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.opendirSync(path, options), callback);
}

// read has a complex polymorphic signature so this is a bit gnarly.
// The various signatures include:
//   fs.read(fd, buffer, offset, length, position, callback)
//   fs.read(fd, callback)
//   fs.read(fd, buffer, callback)
//   fs.read(fd, buffer, { offset, length, position }, callback)
//   fs.read(fd, { buffer, offset, length, position }, callback)
//
// Where fd is always a number, buffer is an ArrayBufferView, offset and
// length are numbers, but position can be a number or bigint, and offset
// length, and position are optional. The callback is always a function
// that receives three arguments: err, bytesRead, and buffer.
export function read<T extends NodeJS.ArrayBufferView>(
  fd: number,
  bufferOptionsOrCallback:
    | T
    | ReadAsyncOptions<T>
    | DoubleArgCallback<number, T>,
  offsetOptionsOrCallback?:
    | ReadAsyncOptions<T>
    | number
    | DoubleArgCallback<number, T>,
  lengthOrCallback?: null | number | DoubleArgCallback<number, T>,
  position?: number | bigint | null,
  callback?: DoubleArgCallback<number, T>
): void {
  // Node.js... you're killing me here with these polymorphic signatures.
  //
  // We're going to normalize the arguments so that we can defer to the
  // readSync variant using the signature readSync(fd, buffer, options)
  //
  // The callback is always the last argument but may appear in the second,
  // third, fourth, or sixth position depending on the signature used. When we
  // find it, we can ignore the remaining arguments that come after it,
  // defaulting any missing arguments to whatever default is defined for
  // them.
  //
  // The second argument is always either a buffer, an options object that
  // contains a buffer property, or the callback. If it's the callback,
  // then we will allocate a new buffer for the read with size 16384 bytes,
  // and default the offset to 0, length to the buffer size, and position to
  // null (indicating that the internal read position for the fd is to be
  // used). If it's an options object, we will use the buffer property from it
  // if it exists. If the buffer property is not present, we will allocate a
  // new buffer for the read with size 16384 bytes. The offset, length, and
  // position properties will be used if they are present in the options
  // object or defaulted to 0, the buffer size, and null respectively.
  // If the second argument is a buffer, we will use it and look for the
  // offset, length, position, and callback arguments in the remaining arguments.
  //
  // The third argument is either ignored (if the second argument is the
  // callback), or it is one of either the offset, the options object, or
  // the callback. If it is the callback, we will default the offset to 0,
  // length to the buffer size (that had to have been provided by the second
  // argument), and position to null. If it is the options object, we will
  // get the offset, length, and position properties from it if they exist,
  // or default them to 0, the buffer size, and null respectively. If it is
  // the offset, we will look for the length, position, and callback in the
  // remaining arguments.
  //
  // The fourth argument is either ignored (if the second or third argument is
  // the callback), or it is the length as either a number, null, or explicitly
  // passed as undefined, or it is the callback. If it is the callback, we will
  // default the length to the buffer size (that had to have been provided by
  // the second argument), and default position to null, then look for the
  // callback in the sixth argument. If it is the length, we will look for the
  // position and callback in the remaining arguments.
  //
  // The fifth argument is either ignored (if the callback has already been
  // seen) or it is the position as either a number, bigint, null, or explicitly
  // undefined. Any other type in this position is an error.
  //
  // The sixth argument is either ignored (if the callback has already been
  // seen) or it is the callback. If it is not a function then an error is
  // thrown.
  //
  // Once we have collected all of the arguments, we will call the readSync
  // method with signature readSync(fd, buffer, { offset, length, position })
  // and pass the return value, and the buffer, to the Node.js-style callback
  // with the signature callback(null, returnValue, buffer). If the call throws,
  // then we will pass the error to the callback as the first argument.

  let actualCallback: undefined | DoubleArgCallback<number, T>;
  let actualBuffer: T; // Buffer, TypedArray, or DataView
  let actualOffset = 0; // Offset from the beginning of the buffer
  let actualLength: number; // Length of the data to read into the buffer
  // Should never be negative and never extends
  // beyond the end of the buffer (that is,
  // actualOffset + actualLength <= actualBuffer.byteLength)
  let actualPosition: number | bigint | null = null; // The position within the
  // file to read from. If null,
  // the current position for the fd
  // is used.

  // Handle the case where the second argument is the callback
  if (typeof bufferOptionsOrCallback === 'function') {
    actualCallback = bufferOptionsOrCallback;
    // Default buffer size when not provided
    // The use of as unknown as T here is a bit of a hack to satisfy the types...
    actualBuffer = Buffer.alloc(16384) as unknown as T;
    actualLength = actualBuffer.byteLength;
  }
  // Handle the case where the second argument is an options object
  else if (
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    bufferOptionsOrCallback != null &&
    typeof bufferOptionsOrCallback === 'object' &&
    !isArrayBufferView(bufferOptionsOrCallback)
  ) {
    // It's an options object
    const {
      buffer = Buffer.alloc(16384),
      offset = buffer.byteOffset,
      length = buffer.byteLength,
      position = null,
    } = bufferOptionsOrCallback;
    if (!isArrayBufferView(buffer)) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.buffer',
        ['Buffer', 'TypedArray', 'DataView'],
        buffer
      );
    }
    validateUint32(offset, 'options.offset');
    validateUint32(length, 'options.length');
    validatePosition(position, 'options.position');

    actualBuffer = buffer as unknown as T;
    actualOffset = offset;
    actualLength = length;
    actualPosition = position;

    // The callback must be in the third argument
    if (typeof offsetOptionsOrCallback !== 'function') {
      throw new ERR_INVALID_ARG_TYPE(
        'callback',
        ['function'],
        offsetOptionsOrCallback
      );
    }
    actualCallback = offsetOptionsOrCallback;
  }
  // Handle the case where the second argument is a buffer
  else {
    actualBuffer = bufferOptionsOrCallback;

    if (!isArrayBufferView(actualBuffer)) {
      throw new ERR_INVALID_ARG_TYPE(
        'buffer',
        ['Buffer', 'TypedArray', 'DataView'],
        actualBuffer
      );
    }

    actualLength = actualBuffer.byteLength;
    actualOffset = actualBuffer.byteOffset;

    // Now we need to find the callback and other parameters
    if (typeof offsetOptionsOrCallback === 'function') {
      // fs.read(fd, buffer, callback)
      actualCallback = offsetOptionsOrCallback;
    } else if (
      typeof offsetOptionsOrCallback === 'object' &&
      !(offsetOptionsOrCallback instanceof Number)
    ) {
      // fs.read(fd, buffer, options, callback)
      const {
        offset = actualOffset,
        length = actualLength,
        position = null,
      } = offsetOptionsOrCallback;
      validateUint32(offset, 'options.offset');
      validateUint32(length, 'options.length');
      validatePosition(position, 'options.position');
      actualOffset = offset;
      actualLength = length;
      actualPosition = position;

      // The callback must be in the fourth argument.
      if (typeof lengthOrCallback !== 'function') {
        throw new ERR_INVALID_ARG_TYPE(
          'callback',
          ['function'],
          lengthOrCallback
        );
      }
      actualCallback = lengthOrCallback;
    } else {
      // fs.read(fd, buffer, offset, length, position, callback)
      actualOffset =
        typeof offsetOptionsOrCallback === 'number'
          ? offsetOptionsOrCallback
          : 0;

      if (typeof lengthOrCallback === 'function') {
        actualCallback = lengthOrCallback;
        actualPosition = null;
        actualLength = actualBuffer.byteLength;
      } else {
        actualLength = lengthOrCallback ?? actualBuffer.byteLength;

        validateUint32(position, 'position');
        actualPosition = position;

        actualCallback = callback;
      }
    }
  }

  // We know that the function must be called with at least 3 arguments and
  // that the first argument is always a number (the fd) and the last must
  // always be the callback.
  // If the actualCallback is not set at this point, then we have a problem.
  if (typeof actualCallback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], actualCallback);
  }

  // We also have a problem if the actualBuffer is not set here correctly.
  if (!isArrayBufferView(actualBuffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['Buffer', 'TypedArray', 'DataView'],
      actualBuffer
    );
  }

  // At this point we have the following:
  // - actualBuffer: The buffer to read into
  // - actualOffset: The offset into the buffer to start writing at
  // - actualLength: The length of the data to read into the buffer
  // - actualPosition: The position within the file to read from (or null)
  // - actualCallback: The callback to call when done
  // - fd: The file descriptor to read from
  // Let actualOffset + actualLength should never be greater than the
  // buffer size. Let's check that.
  if (
    actualOffset < 0 ||
    actualLength < 0 ||
    actualOffset + actualLength > actualBuffer.byteLength
  ) {
    throw new ERR_INVALID_ARG_VALUE(
      'offset',
      'must be >= 0 and <= buffer.length'
    );
  }
  // The actualOffset, actualLength, and actualPosition values should always
  // be greater or equal to 0 (unless actualPosition is null)... keeping in
  // mind that actualPosition can be a number or a bigint.

  // As a special case, if the actualBuffer length is 0, or if actualLength
  // is 0, then can just call the callback with 0 bytes read and return.
  if (actualBuffer.byteLength === 0 || actualLength === 0) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => actualCallback(null, 0, actualBuffer));
    return;
  }

  // Now that we've normalized all the parameters, call readSync
  try {
    const bytesRead = fssync.readSync(fd, actualBuffer, {
      offset: actualOffset,
      length: actualLength,
      position: actualPosition,
    });
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => actualCallback(null, bytesRead, actualBuffer));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => actualCallback(err));
  }
}

export function readdir(
  path: FilePath,
  callback: SingleArgCallback<ReadDirResult>
): void;
export function readdir(
  path: FilePath,
  options: ReadDirOptions,
  callback: SingleArgCallback<ReadDirResult>
): void;
export function readdir(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<ReadDirResult> | ReadDirOptions,
  callback?: SingleArgCallback<ReadDirResult>
): void {
  let options: ReadDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      withFileTypes: false,
      recursive: false,
    };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.readdirSync(path, options), callback);
}

export function readFile(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<string | Buffer> | ReadFileSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: ReadFileSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.readFileSync(path, options), callback);
}

export function readlink(
  path: FilePath,
  callback: SingleArgCallback<string | Buffer>
): void;
export function readlink(
  path: FilePath,
  options: ReadLinkSyncOptions,
  callback: SingleArgCallback<string | Buffer>
): void;
export function readlink(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<string | Buffer> | ReadLinkSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: ReadLinkSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.readlinkSync(path, options), callback);
}

export function readv<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffers: T[],
  positionOrCallback:
    | undefined
    | null
    | bigint
    | number
    | DoubleArgCallback<number, T[]>,
  callback?: DoubleArgCallback<number, T[]>
): void {
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    positionOrCallback = null;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }

  validatePosition(positionOrCallback, 'position');

  try {
    const read = fssync.readvSync(fd, buffers, positionOrCallback);
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(null, read, buffers));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(err));
  }
}

export function realpath(
  path: FilePath,
  callback: SingleArgCallback<string | Buffer>
): void;
export function realpath(
  path: FilePath,
  options: ReadLinkSyncOptions,
  callback: SingleArgCallback<string | Buffer>
): void;
export function realpath(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<string | Buffer> | ReadLinkSyncOptions,
  callback?: SingleArgCallback<string | Buffer>
): void {
  let options: ReadLinkSyncOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.realpathSync(path, options), callback);
}

realpath.native = realpath;

export function rename(
  oldPath: FilePath,
  newPath: FilePath,
  callback: ErrorOnlyCallback
): void {
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.renameSync(oldPath, newPath),
    callback
  );
}

export function rmdir(path: FilePath, callback: ErrorOnlyCallback): void;
export function rmdir(
  path: FilePath,
  options: RmDirOptions,
  callback: ErrorOnlyCallback
): void;
export function rmdir(
  path: FilePath,
  optionsOrCallback: ErrorOnlyCallback | RmDirOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: RmDirOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.rmdirSync(path, options), callback);
}

export function rm(path: FilePath, callback: ErrorOnlyCallback): void;
export function rm(
  path: FilePath,
  options: RmOptions,
  callback: ErrorOnlyCallback
): void;
export function rm(
  path: FilePath,
  optionsOrCallback: ErrorOnlyCallback | RmOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: RmOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {};
  } else {
    options = optionsOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.rmSync(path, options), callback);
}

export function stat(
  path: FilePath,
  callback: SingleArgCallback<Stats | undefined>
): void;
export function stat(
  path: FilePath,
  options: StatOptions,
  callback: SingleArgCallback<Stats | undefined>
): void;
export function stat(
  path: FilePath,
  optionsOrCallback: SingleArgCallback<Stats | undefined> | StatOptions,
  callback?: SingleArgCallback<Stats | undefined>
): void {
  let options: StatOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.statSync(path, options), callback);
}

export function statfs(
  path: FilePath,
  callback: SingleArgCallback<StatsFs | BigIntStatsFs>
): void;
export function statfs(
  path: FilePath,
  options: { bigint: boolean },
  callback: SingleArgCallback<StatsFs | BigIntStatsFs>
): void;
export function statfs(
  path: FilePath,
  optionsOrCallback:
    | SingleArgCallback<StatsFs | BigIntStatsFs>
    | { bigint?: boolean },
  callback?: SingleArgCallback<StatsFs | BigIntStatsFs>
): void {
  let options: { bigint?: boolean };
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = { bigint: false };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback(() => fssync.statfsSync(path, options), callback);
}

export function symlink(
  target: FilePath,
  path: FilePath,
  callback: ErrorOnlyCallback
): void;
export function symlink(
  target: FilePath,
  path: FilePath,
  type: string,
  callback: ErrorOnlyCallback
): void;
export function symlink(
  target: FilePath,
  path: FilePath,
  typeOrCallback: string | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let type: string | null;
  if (typeof typeOrCallback === 'function') {
    callback = typeOrCallback;
    type = null;
  } else {
    type = typeOrCallback;
  }
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.symlinkSync(target, path, type),
    callback
  );
}

export function truncate(path: FilePath, callback: ErrorOnlyCallback): void;
export function truncate(
  path: FilePath,
  len: number,
  callback: ErrorOnlyCallback
): void;
export function truncate(
  path: FilePath,
  lenOrCallback: number | ErrorOnlyCallback,
  callback?: ErrorOnlyCallback
): void {
  let len: number;
  if (typeof lenOrCallback === 'function') {
    callback = lenOrCallback;
    len = 0;
  } else {
    len = lenOrCallback;
  }
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.truncateSync(path, len), callback);
}

export function unlink(path: FilePath, callback: ErrorOnlyCallback): void {
  // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
  callWithErrorOnlyCallback(() => fssync.unlinkSync(path), callback);
}

export function utimes(
  path: FilePath,
  atime: string | number | Date,
  mtime: string | number | Date,
  callback: ErrorOnlyCallback
): void {
  callWithErrorOnlyCallback(
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    () => fssync.utimesSync(path, atime, mtime),
    callback
  );
}

export function write<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffer: T | string,
  offsetOptionsPositionOrCallback?:
    | null
    | WriteSyncOptions
    | number
    | bigint
    | DoubleArgCallback<number, T>,
  encodingLengthOrCallback?:
    | number
    | BufferEncoding
    | DoubleArgCallback<number, T>,
  positionOrCallback?: null | bigint | number | DoubleArgCallback<number, T>,
  callback?: DoubleArgCallback<number, T>
): void {
  if (typeof offsetOptionsPositionOrCallback === 'function') {
    callback = offsetOptionsPositionOrCallback;
    offsetOptionsPositionOrCallback = undefined;
  }
  if (typeof encodingLengthOrCallback === 'function') {
    callback = encodingLengthOrCallback;
    encodingLengthOrCallback = undefined;
  }
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    positionOrCallback = undefined;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  // Because the callback expects the buffer to be returned in the callback,
  // we need to make sure that the buffer is not a string here rather than
  // relying on the transformation in the writeSync call.
  if (typeof buffer === 'string') {
    let encoding = 'utf8';
    if (typeof encodingLengthOrCallback === 'string') {
      encoding = encodingLengthOrCallback;
    }
    buffer = Buffer.from(buffer, encoding) as unknown as T;
  }
  try {
    const written = fssync.writeSync(
      fd,
      buffer,
      offsetOptionsPositionOrCallback,
      encodingLengthOrCallback,
      positionOrCallback
    );
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(null, written, buffer as unknown as T));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(err));
  }
}

export function writeFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  callback: ErrorOnlyCallback
): void;
export function writeFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions,
  callback: ErrorOnlyCallback
): void;
export function writeFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  optionsOrCallback: ErrorOnlyCallback | WriteFileOptions,
  callback?: ErrorOnlyCallback
): void {
  let options: WriteFileOptions;
  if (typeof optionsOrCallback === 'function') {
    callback = optionsOrCallback;
    options = {
      encoding: 'utf8',
      mode: 0o666,
      flag: 'w',
      flush: false,
    };
  } else {
    options = optionsOrCallback;
  }
  callWithSingleArgCallback<number>(
    () => fssync.writeFileSync(path, data, options),
    callback
  );
}

export function writev<T extends NodeJS.ArrayBufferView>(
  fd: number,
  buffers: T[],
  positionOrCallback?: null | number | bigint | DoubleArgCallback<number, T[]>,
  callback?: DoubleArgCallback<number, T[]>
): void {
  if (typeof positionOrCallback === 'function') {
    callback = positionOrCallback;
    positionOrCallback = null;
  }
  if (typeof callback !== 'function') {
    throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
  }
  try {
    const written = fssync.writevSync(fd, buffers, positionOrCallback);
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(null, written, buffers));
  } catch (err) {
    // eslint-disable-next-line @typescript-eslint/no-confusing-void-expression
    queueMicrotask(() => callback(err));
  }
}

export function unwatchFile(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function watch(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function watchFile(): void {
  // We currently do not implement file watching.
  throw new Error('Not implemented');
}

export function createReadStream(): void {
  throw new Error('Not implemented');
}
export function createWriteStream(): void {
  throw new Error('Not implemented');
}

// eslint-enable @typescript-eslint/no-confusing-void-expression

// An API is considered stubbed if it is not implemented by the function
// exists with the correct signature and throws an error if called. If
// a function exists that does not have the correct signature, it is
// not considered fully stubbed.
// An API is considered optimized if the API has been implemented and
// tested and then optimized for performance.
// Implemented APIs here are a bit different than in the sync version
// since most of these are implemented in terms of calling the sync
// version. We consider it implemented here if the code is present and
// calls the sync api even if the sync api itself it not fully implemented.
//
// (S == Stubbed, I == Implemented, T == Tested, O == Optimized)
//  S  I  T  O
// [x][x][x][ ] fs.access(path[, mode], callback)
// [x][x][ ][ ] fs.appendFile(path, data[, options], callback)
// [x][x][x][ ] fs.chmod(path, mode, callback)
// [x][x][x][ ] fs.chown(path, uid, gid, callback)
// [x][x][x][ ] fs.close(fd[, callback])
// [x][x][ ][ ] fs.copyFile(src, dest[, mode], callback)
// [x][x][ ][ ] fs.cp(src, dest[, options], callback)
// [ ][ ][ ][ ] fs.createReadStream(path[, options])
// [ ][ ][ ][ ] fs.createWriteStream(path[, options])
// [x][x][x][ ] fs.exists(path, callback)
// [x][x][x][ ] fs.fchmod(fd, mode, callback)
// [x][x][x][ ] fs.fchown(fd, uid, gid, callback)
// [x][x][x][ ] fs.fdatasync(fd, callback)
// [x][x][x][ ] fs.fstat(fd[, options], callback)
// [x][x][x][ ] fs.fsync(fd, callback)
// [x][x][x][ ] fs.ftruncate(fd[, len], callback)
// [x][x][x][ ] fs.futimes(fd, atime, mtime, callback)
// [ ][ ][ ][ ] fs.glob(pattern[, options], callback)
// [x][x][x][ ] fs.lchmod(path, mode, callback)
// [x][x][x][ ] fs.lchown(path, uid, gid, callback)
// [x][x][x][ ] fs.lutimes(path, atime, mtime, callback)
// [x][x][x][ ] fs.link(existingPath, newPath, callback)
// [x][x][x][ ] fs.lstat(path[, options], callback)
// [x][x][ ][ ] fs.mkdir(path[, options], callback)
// [x][x][ ][ ] fs.mkdtemp(prefix[, options], callback)
// [x][x][x][ ] fs.open(path[, flags[, mode]], callback)
// [ ][ ][ ][ ] fs.openAsBlob(path[, options])
// [x][x][ ][ ] fs.opendir(path[, options], callback)
// [x][x][ ][ ] fs.read(fd, buffer, offset, length, position, callback)
// [x][x][ ][ ] fs.read(fd[, options], callback)
// [x][x][ ][ ] fs.read(fd, buffer[, options], callback)
// [x][x][ ][ ] fs.readdir(path[, options], callback)
// [x][x][ ][ ] fs.readFile(path[, options], callback)
// [x][x][x][ ] fs.readlink(path[, options], callback)
// [x][x][ ][ ] fs.readv(fd, buffers[, position], callback)
// [x][x][x][ ] fs.realpath(path[, options], callback)
// [x][x][x][ ] fs.realpath.native(path[, options], callback)
// [x][x][ ][ ] fs.rename(oldPath, newPath, callback)
// [x][x][ ][ ] fs.rmdir(path[, options], callback)
// [x][x][ ][ ] fs.rm(path[, options], callback)
// [x][x][x][ ] fs.stat(path[, options], callback)
// [x][x][x][ ] fs.statfs(path[, options], callback)
// [x][x][x][ ] fs.symlink(target, path[, type], callback)
// [x][x][x][ ] fs.truncate(path[, len], callback)
// [x][x][x][ ] fs.unlink(path, callback)
// [ ][ ][ ][ ] fs.unwatchFile(filename[, listener])
// [x][x][x][ ] fs.utimes(path, atime, mtime, callback)
// [ ][ ][ ][ ] fs.watch(filename[, options][, listener])
// [ ][ ][ ][ ] fs.watchFile(filename[, options], listener)
// [x][x][ ][ ] fs.write(fd, buffer, offset[, length[, position]], callback)
// [x][x][ ][ ] fs.write(fd, buffer[, options], callback)
// [x][x][ ][ ] fs.write(fd, string[, position[, encoding]], callback)
// [x][x][ ][ ] fs.writeFile(file, data[, options], callback)
// [x][x][ ][ ] fs.writev(fd, buffers[, position], callback)
