// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import * as fssync from 'node-internal:internal_fs_sync';
import { EventEmitter } from 'node-internal:events';
import { default as cffs } from 'cloudflare-internal:filesystem';
import type {
  MkdirTempSyncOptions,
  ReadDirResult,
  ReadFileSyncOptions,
  ReadLinkSyncOptions,
  StatOptions,
} from 'node-internal:internal_fs_sync';
import {
  type ReadStream,
  type ReadStreamOptions,
  type WriteStream,
  type WriteStreamOptions,
  createReadStream,
  createWriteStream,
} from 'node-internal:internal_fs_streams';
import {
  kBadge,
  kFileHandle,
  Stats,
  validatePosition,
  type Position,
  type RawTime,
  type SymlinkType,
  type FilePath,
  type ReadDirOptions,
  type WriteSyncOptions,
  type ValidEncoding,
} from 'node-internal:internal_fs_utils';
import type { Dirent } from 'node-internal:internal_fs';
import { Buffer } from 'node-internal:internal_buffer';
import { type Dir } from 'node-internal:internal_fs';
import {
  ERR_EBADF,
  ERR_UNSUPPORTED_OPERATION,
} from 'node-internal:internal_errors';
import { validateUint32 } from 'node-internal:validators';
import * as constants from 'node-internal:internal_fs_constants';
import type {
  BigIntStatsFs,
  CopySyncOptions,
  GlobOptions,
  GlobOptionsWithFileTypes,
  GlobOptionsWithoutFileTypes,
  MakeDirectoryOptions,
  OpenDirOptions,
  ReadAsyncOptions,
  RmOptions,
  RmDirOptions,
  StatsFs,
  WriteFileOptions,
} from 'node:fs';
import type {
  ReadableWebStreamOptions,
  CreateReadStreamOptions,
} from 'node:fs/promises';
import { isArrayBufferView } from 'node-internal:internal_types';

export class FileHandle extends EventEmitter {
  // The FileHandle class is a wrapper around a file descriptor.
  // When the #handle is cleared, the reference to the underlying
  // file descriptor is dropped. The user is expected to call
  // close() explicitly but if they do not, the file descriptor
  // will still be closed when the underlying handle is garbage
  // collected.
  #fd: number | undefined;
  #handle: cffs.FdHandle | undefined;
  [kFileHandle] = true;

  constructor(badge: symbol, fd: number) {
    if (badge !== kBadge) {
      throw new TypeError('Illegal constructor');
    }
    super();
    this.#fd = fd;
    this.#handle = cffs.getFdHandle(fd);
  }

  get fd(): number | undefined {
    // The fd property will be undefined if the handle has been closed.
    return this.#fd;
  }

  async appendFile(
    data: string | ArrayBufferView,
    options: WriteFileOptions = {}
  ): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await appendFile(this.#fd, data, options);
  }

  async chmod(mode: string | number): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await fchmod(this.#fd, mode);
  }

  async chown(uid: number, gid: number): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await fchown(this.#fd, uid, gid);
  }

  async datasync(): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await fdatasync(this.#fd);
  }

  async sync(): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await fsync(this.#fd);
  }

  read<T extends NodeJS.ArrayBufferView>(
    bufferOrOptions: T | ReadAsyncOptions<T> = {},
    offsetOrOptions: number | ReadAsyncOptions<T> = {},
    length?: number,
    position: Position = null
  ): Promise<{ bytesRead: number; buffer: T }> {
    try {
      if (this.#fd === undefined) {
        throw new ERR_EBADF({ syscall: 'stat' });
      }

      let options: ReadAsyncOptions<T>;
      if (isArrayBufferView(bufferOrOptions)) {
        if (typeof offsetOrOptions === 'number') {
          options = {
            buffer: bufferOrOptions,
            offset: offsetOrOptions,
            length: length ?? bufferOrOptions.byteLength,
            position: position ?? null,
          };
        } else {
          options = {
            buffer: bufferOrOptions,
            ...offsetOrOptions,
          };
        }
      } else {
        options = bufferOrOptions;
      }

      const {
        buffer = Buffer.alloc(16384),
        offset: actualOffset = buffer.byteOffset,
        length: actualLength = buffer.byteLength - buffer.byteOffset,
        position: actualPosition = null,
      } = options;

      validateUint32(actualOffset, 'offset');
      validateUint32(actualLength, 'length');
      validatePosition(actualPosition, 'position');

      const bytesRead = fssync.readSync(
        this.#fd,
        buffer as NodeJS.ArrayBufferView,
        actualOffset,
        actualLength,
        actualPosition
      );

      return Promise.resolve({
        bytesRead,
        buffer: buffer as T,
      });
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  readv<T extends NodeJS.ArrayBufferView>(
    buffers: T[],
    position: Position = null
  ): Promise<{ bytesRead: number; buffers: T[] }> {
    try {
      if (this.#fd === undefined) {
        throw new ERR_EBADF({ syscall: 'stat' });
      }
      const bytesRead = fssync.readvSync(this.#fd, buffers, position);
      return Promise.resolve({
        bytesRead,
        buffers,
      });
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  async readFile(
    options: ValidEncoding | ReadFileSyncOptions = {}
  ): Promise<string | Buffer> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    return await readFile(this.#fd, options);
  }

  readLines(_options: CreateReadStreamOptions = {}): void {
    // TODO(node-fs): Implement
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    throw new Error('not implemented');
  }

  async stat(options: StatOptions = {}): Promise<Stats | undefined> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    return await fstat(this.#fd, options);
  }

  async truncate(len: number = 0): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await ftruncate(this.#fd, len);
  }

  async utimes(atime: RawTime | Date, mtime: RawTime | Date): Promise<void> {
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    await futimes(this.#fd, atime, mtime);
  }

  write(
    buffer: NodeJS.ArrayBufferView | string,
    offsetPositionOrOptions: WriteSyncOptions | Position = null,
    lengthOrEncoding?: number | ValidEncoding,
    position: Position = null
  ): Promise<{ bytesWritten: number; buffer: NodeJS.ArrayBufferView }> {
    try {
      if (this.#fd === undefined) {
        throw new ERR_EBADF({ syscall: 'stat' });
      }

      if (typeof buffer === 'string') {
        buffer = Buffer.from(buffer, lengthOrEncoding as string);
      }

      const bytesWritten = fssync.writeSync(
        this.#fd,
        buffer,
        offsetPositionOrOptions,
        lengthOrEncoding,
        position
      );

      return Promise.resolve({
        bytesWritten,
        buffer,
      });
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  writev(
    buffers: NodeJS.ArrayBufferView[],
    position: Position = null
  ): Promise<{ bytesWritten: number; buffers: NodeJS.ArrayBufferView[] }> {
    try {
      if (this.#fd === undefined) {
        throw new ERR_EBADF({ syscall: 'stat' });
      }
      const bytesWritten = fssync.writevSync(this.#fd, buffers, position);
      return Promise.resolve({
        bytesWritten,
        buffers,
      });
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  writeFile(
    data: string | Buffer,
    options: ValidEncoding | WriteFileOptions = {}
  ): Promise<{ bytesWritten: number; buffer: Buffer }> {
    try {
      if (this.#fd === undefined) {
        throw new ERR_EBADF({ syscall: 'stat' });
      }
      const bytesWritten = fssync.writeFileSync(this.#fd, data, options);
      return Promise.resolve({
        bytesWritten,
        buffer: isArrayBufferView(data)
          ? data
          : Buffer.from(data, options as BufferEncoding),
      });
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  close(): Promise<void> {
    try {
      this.#handle?.close();
      this.#fd = undefined;
      this.#handle = undefined;
      (this as unknown as EventEmitter).emit('close');
      return Promise.resolve();
    } catch (err) {
      return Promise.reject(err as Error);
    }
  }

  async [Symbol.asyncDispose](): Promise<void> {
    await this.close();
  }

  readableWebStream(_options: ReadableWebStreamOptions = {}): void {
    // TODO(node-fs): Implement
    if (this.#fd === undefined) {
      throw new ERR_EBADF({ syscall: 'stat' });
    }
    throw new Error('not implemented');
  }

  createReadStream(options: ReadStreamOptions = {}): ReadStream {
    return createReadStream('', { ...options, fd: this });
  }

  createWriteStream(_options: WriteStreamOptions = {}): WriteStream {
    return createWriteStream('', { ..._options, fd: this });
  }
}

export function access(
  path: FilePath,
  mode: number = constants.F_OK
): Promise<void> {
  // Unlike the callback version, which throws input validation errors synchronously,
  // rather than forwarding them to the callback, the promise version throws a errors
  // asynchronously by rejecting the promise. So we can rely on accessSync to do the
  // input validation for us.
  return Promise.try(() => {
    fssync.accessSync(path, mode);
  });
}

export function appendFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: WriteFileOptions = {}
): Promise<void> {
  return Promise.try(() => {
    // While appendFileSync returns the number of bytes written,
    // the promise version does not return anything when successful.
    fssync.appendFileSync(path, data, options);
  });
}

export function chmod(path: FilePath, mode: number): Promise<void> {
  return Promise.try(() => {
    fssync.chmodSync(path, mode);
  });
}

export function chown(path: FilePath, uid: number, gid: number): Promise<void> {
  return Promise.try(() => {
    fssync.chownSync(path, uid, gid);
  });
}

export function copyFile(
  src: FilePath,
  dest: FilePath,
  mode: number
): Promise<void> {
  return Promise.try(() => {
    fssync.copyFileSync(src, dest, mode);
  });
}

export function cp(
  src: FilePath,
  dest: FilePath,
  options: CopySyncOptions
): Promise<void> {
  return Promise.try(() => {
    fssync.cpSync(src, dest, options);
  });
}

function fchmod(fd: number, mode: string | number): Promise<void> {
  return Promise.try(() => {
    fssync.fchmodSync(fd, mode);
  });
}

function fchown(fd: number, uid: number, gid: number): Promise<void> {
  return Promise.try(() => {
    fssync.fchownSync(fd, uid, gid);
  });
}

function fdatasync(fd: number): Promise<void> {
  return Promise.try(() => {
    fssync.fdatasyncSync(fd);
  });
}

function fsync(fd: number): Promise<void> {
  return Promise.try(() => {
    fssync.fsyncSync(fd);
  });
}

function fstat(
  fd: number,
  options: StatOptions = {}
): Promise<Stats | undefined> {
  return Promise.try(() => {
    return fssync.fstatSync(fd, options);
  });
}

function ftruncate(fd: number, len: number = 0): Promise<void> {
  return Promise.try(() => {
    fssync.ftruncateSync(fd, len);
  });
}

function futimes(
  fd: number,
  atime: RawTime | Date,
  mtime: RawTime | Date
): Promise<void> {
  return Promise.try(() => {
    fssync.futimesSync(fd, atime, mtime);
  });
}

export function lchmod(path: FilePath, mode: number): Promise<void> {
  return Promise.try(() => {
    fssync.lchmodSync(path, mode);
  });
}

export function lchown(
  path: FilePath,
  uid: number,
  gid: number
): Promise<void> {
  return Promise.try(() => {
    fssync.lchownSync(path, uid, gid);
  });
}

export function lutimes(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date
): Promise<void> {
  return Promise.try(() => {
    fssync.lutimesSync(path, atime, mtime);
  });
}

export function link(existingPath: FilePath, newPath: FilePath): Promise<void> {
  return Promise.try(() => {
    fssync.linkSync(existingPath, newPath);
  });
}

export function lstat(
  path: FilePath,
  options: StatOptions = {}
): Promise<Stats | undefined> {
  return Promise.try(() => {
    return fssync.lstatSync(path, options);
  });
}

export function mkdir(
  path: FilePath,
  options: number | MakeDirectoryOptions = {}
): Promise<string | undefined> {
  return Promise.try(() => fssync.mkdirSync(path, options));
}

export function mkdtemp(
  prefix: FilePath,
  options: MkdirTempSyncOptions = {}
): Promise<string> {
  return Promise.try(() => fssync.mkdtempSync(prefix, options));
}

export function open(
  path: FilePath,
  flags: number | string,
  mode: number | string
): Promise<FileHandle> {
  return Promise.try(
    () => new FileHandle(kBadge, fssync.openSync(path, flags, mode))
  );
}

export function opendir(
  path: FilePath,
  options: OpenDirOptions = {}
): Promise<Dir> {
  return Promise.try(() => fssync.opendirSync(path, options));
}

export function readdir(
  path: FilePath,
  options: ReadDirOptions = {}
): Promise<ReadDirResult> {
  return Promise.try(() => fssync.readdirSync(path, options));
}

export function readFile(
  path: number | FilePath,
  options: ValidEncoding | ReadFileSyncOptions = {}
): Promise<string | Buffer> {
  return Promise.try(() => fssync.readFileSync(path, options));
}

export function readlink(
  path: FilePath,
  options: ValidEncoding | ReadLinkSyncOptions = {}
): Promise<string | Buffer> {
  return Promise.try(() => fssync.readlinkSync(path, options));
}

export function realpath(
  path: FilePath,
  options: ValidEncoding | ReadLinkSyncOptions = {}
): Promise<string | Buffer> {
  return Promise.try(() => fssync.realpathSync(path, options));
}

export function rename(oldPath: FilePath, newPath: FilePath): Promise<void> {
  return Promise.try(() => {
    fssync.renameSync(oldPath, newPath);
  });
}

export function rmdir(path: FilePath, options: RmDirOptions): Promise<void> {
  return Promise.try(() => {
    fssync.rmdirSync(path, options);
  });
}

export function rm(path: FilePath, options: RmOptions = {}): Promise<void> {
  return Promise.try(() => {
    fssync.rmSync(path, options);
  });
}

export function stat(
  path: FilePath,
  options: StatOptions = {}
): Promise<Stats | undefined> {
  return Promise.try(() => fssync.statSync(path, options));
}

export function statfs(
  path: FilePath,
  options: { bigint?: boolean | undefined } = {}
): Promise<StatsFs | BigIntStatsFs> {
  return Promise.try(() => fssync.statfsSync(path, options));
}

export function symlink(
  target: FilePath,
  path: FilePath,
  type: SymlinkType = null
): Promise<void> {
  return Promise.try(() => {
    fssync.symlinkSync(target, path, type);
  });
}

export function truncate(path: FilePath, len: number = 0): Promise<void> {
  return Promise.try(() => {
    fssync.truncateSync(path, len);
  });
}

export function unlink(path: FilePath): Promise<void> {
  return Promise.try(() => {
    fssync.unlinkSync(path);
  });
}

export function utimes(
  path: FilePath,
  atime: RawTime | Date,
  mtime: RawTime | Date
): Promise<void> {
  return Promise.try(() => {
    fssync.utimesSync(path, atime, mtime);
  });
}

export function watch(): Promise<void> {
  // We do not implement the watch function.
  throw new Error('Not implemented');
}

export function writeFile(
  path: number | FilePath,
  data: string | ArrayBufferView,
  options: ValidEncoding | WriteFileOptions = {}
): Promise<void> {
  return Promise.try(() => {
    // While writeFileSync returns the number of bytes written,
    // the promise version does not return anything when successful.
    fssync.writeFileSync(path, data, options);
  });
}

export function glob(
  _pattern: string | readonly string[],
  _options:
    | GlobOptions
    | GlobOptionsWithFileTypes
    | GlobOptionsWithoutFileTypes = {}
): NodeJS.AsyncIterator<string | Dirent> {
  // We do not yet implement the globSync function. In Node.js, this
  // function depends heavily on the third party minimatch library
  // which is not yet available in the workers runtime. This will be
  // explored for implementation separately in the future.
  throw new ERR_UNSUPPORTED_OPERATION();
}
