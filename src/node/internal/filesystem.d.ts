export interface StatOptions {
  followSymlinks?: boolean;
}

export interface Stat {
  type: 'file' | 'directory' | 'symlink';
  size: number;
  lastModified: bigint;
  created: bigint;
  writable: boolean;
  device: boolean;
}

export function stat(pathOrFd: number | URL, options: StatOptions): Stat | null;
export function setLastModified(
  pathOrFd: number | URL,
  mtime: Date,
  options: StatOptions
): void;

export function truncate(pathOrFd: number | URL, length: number): void;

export function readLink(
  path: URL,
  options: { failIfNotSymlink: boolean }
): string;

export function link(from: URL, to: URL, options: { symbolic: boolean }): void;

export function unlink(path: URL): void;

export function open(
  path: URL,
  options: {
    read: boolean;
    write: boolean;
    append: boolean;
    exclusive: boolean;
    followSymlinks: boolean;
  }
): number;

export function close(fd: number): void;

export function write(
  fd: number,
  buffers: ArrayBufferView[],
  options: {
    position: number | bigint | null;
  }
): number;

export function read(
  fd: number,
  buffers: ArrayBufferView[],
  options: {
    position: number | bigint | null;
  }
): number;

export function readAll(pathOrFd: number | URL): Uint8Array;

export function writeAll(
  pathOrFd: number | URL,
  data: ArrayBufferView,
  options: { append: boolean; exclusive: boolean }
): number;

export function renameOrCopy(
  from: URL,
  to: URL,
  options: { copy: boolean }
): void;

export function mkdir(
  path: URL,
  options: { recursive: boolean; tmp: boolean }
): string | undefined;

export function rm(
  path: URL,
  options: { recursive: boolean; force: boolean; dironly: boolean }
): void;

export interface DirEntryHandle {
  name: string;
  parentPath: string;
  type: number;
}

export function readdir(
  path: URL,
  options: { recursive: boolean }
): DirEntryHandle[];

export function cp(
  src: URL,
  dest: URL,
  options: {
    deferenceSymlinks: boolean;
    recursive: boolean;
    force: boolean;
    errorOnExist: boolean;
  }
): void;

interface FdHandle {
  close(): void;
}

export function getFdHandle(fd: number): FdHandle;

export interface OpenAsBlobOptions {
  type?: string | undefined;
}

export function openAsBlob(path: URL, options: OpenAsBlobOptions): Blob;
