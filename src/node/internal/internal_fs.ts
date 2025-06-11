import {
  UV_DIRENT_DIR,
  UV_DIRENT_FILE,
  UV_DIRENT_BLOCK,
  UV_DIRENT_CHAR,
  UV_DIRENT_LINK,
  UV_DIRENT_FIFO,
  UV_DIRENT_SOCKET,
} from 'node-internal:internal_fs_constants';
import { getOptions } from 'node-internal:internal_fs_utils';
import { validateFunction, validateUint32 } from 'node-internal:validators';
import { ERR_MISSING_ARGS } from 'node-internal:internal_errors';
import type { Buffer } from 'node-internal:internal_buffer';
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

export class Dir {
  // @ts-expect-error TS6133 Value is not read.
  #handle: unknown; // eslint-disable-line no-unused-private-class-members
  #path: string;
  #options: Record<string, unknown>;

  public constructor(
    handle: unknown,
    path: string,
    options: Record<string, unknown>
  ) {
    if (handle == null) {
      throw new ERR_MISSING_ARGS('handle');
    }
    this.#handle = handle;
    this.#path = path;
    this.#options = {
      bufferSize: 32,
      ...getOptions(options, {
        encoding: 'utf8',
      }),
    };

    validateUint32(this.#options.bufferSize, 'options.bufferSize', true);
  }

  public get path(): string {
    return this.#path;
  }

  public read(callback: unknown): void {
    validateFunction(callback, 'callback');
    throw new Error('Not implemented');
  }

  public processReadResult(): void {
    throw new Error('Not implemented');
  }

  public readSyncRecursive(): void {
    throw new Error('Not implemented');
  }

  public readSync(): void {
    throw new Error('Not implemented');
  }

  public close(): void {
    throw new Error('Not implemented');
  }

  public closeSync(): void {
    throw new Error('Not implemented');
  }

  // eslint-disable-next-line @typescript-eslint/require-await,require-yield
  public async *entries(): AsyncGenerator<unknown, unknown, unknown> {
    throw new Error('Not implemented');
  }
}
