import {
  UV_DIRENT_DIR,
  UV_DIRENT_FILE,
  UV_DIRENT_BLOCK,
  UV_DIRENT_CHAR,
  UV_DIRENT_LINK,
  UV_DIRENT_FIFO,
  UV_DIRENT_SOCKET,
} from 'node-internal:internal_fs_constants';
import { type FilePath } from 'node-internal:internal_fs_utils';
import {
  default as cffs,
  type DirEntryHandle,
} from 'cloudflare-internal:filesystem';
import {
  validateFunction,
  validateObject,
  validateString,
} from 'node-internal:validators';
import {
  ERR_DIR_CLOSED,
  ERR_MISSING_ARGS,
} from 'node-internal:internal_errors';
import { Buffer } from 'node-internal:internal_buffer';
const kType = Symbol('type');

export class Dirent {
  public name: string | Buffer;
  public parentPath: string | Buffer;
  private [kType]: number;

  public constructor(
    name: string | Buffer,
    type: number,
    path: string | Buffer
  ) {
    this.name = name;
    this.parentPath = path;
    this[kType] = type;
  }

  public isDirectory(): boolean {
    return this[kType] === UV_DIRENT_DIR;
  }

  public isFile(): boolean {
    return this[kType] === UV_DIRENT_FILE;
  }

  public isBlockDevice(): boolean {
    return this[kType] === UV_DIRENT_BLOCK;
  }

  public isCharacterDevice(): boolean {
    return this[kType] === UV_DIRENT_CHAR;
  }

  public isSymbolicLink(): boolean {
    return this[kType] === UV_DIRENT_LINK;
  }

  public isFIFO(): boolean {
    return this[kType] === UV_DIRENT_FIFO;
  }

  public isSocket(): boolean {
    return this[kType] === UV_DIRENT_SOCKET;
  }
}

export interface DirOptions {
  encoding?: BufferEncoding | null | undefined | 'buffer';
}

export type DirentReadCallback = (
  err: Error | null,
  dirent: Dirent | null
) => void;

export class Dir {
  // Unlike our Node.js counterpart, we do not not use an underlying
  // native handle for the directory. Instead we use a simple array
  // of DirEntryHandle objects just like the one used in the readdir
  // API.
  #handle: DirEntryHandle[] | undefined;
  #encoding: BufferEncoding | null | undefined | 'buffer';
  #path: FilePath;

  public constructor(
    handle: cffs.DirEntryHandle[] | undefined,
    path: FilePath,
    options: DirOptions
  ) {
    if (handle == null) {
      throw new ERR_MISSING_ARGS('handle');
    }
    this.#handle = handle;
    this.#path = path;

    validateObject(options, 'options');
    const { encoding = 'utf8' } = options;
    validateString(encoding, 'options.encoding');
    this.#encoding = encoding as BufferEncoding;
  }

  public get path(): FilePath {
    return this.#path;
  }

  public read(): Promise<Dirent | null>;
  public read(callback: DirentReadCallback): void;
  public read(
    callback?: DirentReadCallback
  ): Promise<Dirent | null> | undefined {
    if (typeof callback === 'function') {
      validateFunction(callback, 'callback');
      try {
        const ent = this.readSync();
        queueMicrotask(() => {
          callback(null, ent);
        });
      } catch (err) {
        queueMicrotask(() => {
          callback(err as Error, null);
        });
      }
      return;
    }

    try {
      const ent = this.readSync();
      return Promise.resolve(ent);
    } catch (err: unknown) {
      return Promise.reject(err as Error);
    }
  }

  public readSync(): Dirent | null {
    if (this.#handle === undefined) throw new ERR_DIR_CLOSED();
    const ent = this.#handle.shift();
    if (ent == null) return null;
    const buf = Buffer.from(ent.name);
    if (this.#encoding === 'buffer') {
      return new Dirent(buf, ent.type, ent.parentPath);
    }
    return new Dirent(
      buf.toString(this.#encoding != null ? this.#encoding : undefined),
      ent.type,
      ent.parentPath
    );
  }

  public close(callback?: (err: unknown) => void): Promise<void> | undefined {
    this.closeSync();
    if (typeof callback === 'function') {
      validateFunction(callback, 'callback');
      queueMicrotask(() => {
        callback(null);
      });
      return undefined;
    }
    return Promise.resolve();
  }

  public closeSync(): void {
    if (this.#handle === undefined) {
      throw new ERR_DIR_CLOSED();
    }
    this.#handle = undefined;
  }

  public async *entries(): AsyncGenerator<Dirent, unknown, unknown> {
    for (;;) {
      const ent = await this.read();
      if (ent == null) return;
      yield ent;
    }
  }

  public [Symbol.asyncIterator](): AsyncGenerator<Dirent, unknown, unknown> {
    return this.entries();
  }

  public async [Symbol.asyncDispose](): Promise<void> {
    await this.close();
  }

  public [Symbol.dispose](): void {
    this.closeSync();
  }
}
