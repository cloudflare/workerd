export interface StatOptions {
  followSymlinks?: boolean;
}

export interface OpenOptions {
  exclusive: boolean;
  read: boolean;
  write: boolean;
  append: boolean;
}

export interface ReadOptions {
  position?: number | undefined;
}

export interface CopyFileOptions {
  exclusive: boolean;
}

export interface Stat {
  type: 'file' | 'directory' | 'symlink';
  size: number;
  lastModified: bigint;
  created: bigint;
  writable: boolean;
  device: boolean;
}

export interface WriteFileOptions {
  append: boolean;
}

import type { Buffer } from 'node:buffer';

export type FilePath = string | Buffer | URL;

export function stat(pathOrFd: number | URL, options: StatOptions): Stat | null;
export function setLastModified(
  pathOrFd: number | URL,
  time: Date,
  options: StatOptions
): void;
export function resize(pathOrFd: number | URL, size: number): void;
export function link(existingPath: URL, newPath: URL): void;
export function symlink(existingPath: URL, newPath: URL): void;
export function unlink(path: URL): void;
export function realpath(path: URL): string;
export function readlink(path: URL): string;
export function open(path: URL, options: OpenOptions): number;
export function close(fd: number): void;
export function fsync(fd: number): void;
export function readfile(pathOrFd: number | URL): Uint8Array;
export function copyFile(from: URL, to: URL, options: CopyFileOptions): void;
export function rename(from: URL, to: URL): void;
export function writeFile(
  pathOrFd: number | URL,
  data: ArrayBufferView,
  options: WriteFileOptions
): void;
export function readv(
  fd: number,
  buffers: ArrayBufferView[],
  options: ReadOptions
): number;

export function writev(
  fd: number,
  buffers: ArrayBufferView[],
  options: ReadOptions
): number;
