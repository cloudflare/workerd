import * as promises from 'node-internal:internal_fs_promises';
import { callbackify } from 'node-internal:internal_utils';

export const access = callbackify(promises.access.bind(promises));
export const appendFile = callbackify(promises.appendFile.bind(promises));
export const chmod = callbackify(promises.chmod.bind(promises));
export const chown = callbackify(promises.chown.bind(promises));
export function close(): void {
  throw new Error('Not implemented');
}
export const copyFile = callbackify(promises.copyFile.bind(promises));
export const cp = callbackify(promises.cp.bind(promises));
export function createReadStream(): void {
  throw new Error('Not implemented');
}
export function createWriteStream(): void {
  throw new Error('Not implemented');
}
export function fchmod(): void {
  throw new Error('Not implemented');
}
export const fchown = callbackify(promises.copyFile.bind(promises));
export function fdatasync(): void {
  throw new Error('Not implemented');
}
export function fstat(): void {
  throw new Error('Not implemented');
}
export function fsync(): void {
  throw new Error('Not implemented');
}
export function ftruncate(): void {
  throw new Error('Not implemented');
}
export function futimes(): void {
  throw new Error('Not implemented');
}
export const lchmod = callbackify(promises.lchmod.bind(promises));
export const lchown = callbackify(promises.lchown.bind(promises));
export const lutimes = callbackify(promises.lutimes.bind(promises));
export const link = callbackify(promises.link.bind(promises));
export const lstat = callbackify(promises.lstat.bind(promises));
export const mkdir = callbackify(promises.mkdir.bind(promises));
export const mkdtemp = callbackify(promises.mkdtemp.bind(promises));
export const open = callbackify(promises.open.bind(promises));
export const opendir = callbackify(promises.opendir.bind(promises));
export function read(): void {
  throw new Error('Not implemented');
}
export const readdir = callbackify(promises.readdir.bind(promises));
export const readFile = callbackify(promises.readFile.bind(promises));
export const readlink = callbackify(promises.readlink.bind(promises));
export function readv(): void {
  throw new Error('Not implemented');
}
export const realpath = callbackify(promises.realpath.bind(promises));
export const rename = callbackify(promises.rename.bind(promises));
export const rmdir = callbackify(promises.rmdir.bind(promises));
export const rm = callbackify(promises.rm.bind(promises));
export const stat = callbackify(promises.stat.bind(promises));
export const statfs = callbackify(promises.statfs.bind(promises));
export const symlink = callbackify(promises.symlink.bind(promises));
export const truncate = callbackify(promises.truncate.bind(promises));
export const unlink = callbackify(promises.unlink.bind(promises));
export function unwatchFile(): void {
  throw new Error('Not implemented');
}
export const utimes = callbackify(promises.utimes.bind(promises));
export const watch = callbackify(promises.watch.bind(promises));
export function watchFile(): void {
  throw new Error('Not implemented');
}
export function write(): void {
  throw new Error('Not implemented');
}
export const writeFile = callbackify(promises.writeFile.bind(promises));
export function writev(): void {
  throw new Error('Not implemented');
}
