// Copyright (c) 2024 Jeju Network
// Bun FFI Compatibility Layer for Workerd
// Licensed under the Apache 2.0 license

/**
 * bun:ffi Module
 *
 * Foreign Function Interface for calling native code.
 *
 * ⚠️  NOT AVAILABLE IN WORKERD
 *
 * FFI requires direct native code execution which is fundamentally incompatible
 * with workerd's V8 isolate sandbox model. This is a security feature, not a bug.
 *
 * ALTERNATIVES:
 *
 * 1. WebAssembly (WASM) - Recommended
 *    Most native libraries have WASM builds. Use them instead:
 *    ```ts
 *    // Instead of: const lib = dlopen('libcrypto.so', {...})
 *    // Use: import init from '@aspect/crypto-wasm'
 *    const crypto = await init()
 *    ```
 *
 * 2. HTTP Services
 *    For heavy computation, call an external service:
 *    ```ts
 *    const result = await fetch('https://compute.example.com/process', {
 *      method: 'POST',
 *      body: data
 *    })
 *    ```
 *
 * 3. Workers AI (Cloudflare)
 *    For ML/AI workloads, use Workers AI bindings.
 *
 * Common WASM alternatives:
 * - bcryptjs (pure JS bcrypt) - already included in this bundle
 * - argon2-browser (WASM argon2)
 * - @aspect/crypto-wasm (general crypto)
 * - @aspect/sqlite-wasm (SQLite) - or use SQLit HTTP client
 */

export type FFIType =
  | 'void'
  | 'bool'
  | 'char'
  | 'i8'
  | 'i16'
  | 'i32'
  | 'i64'
  | 'u8'
  | 'u16'
  | 'u32'
  | 'u64'
  | 'f32'
  | 'f64'
  | 'pointer'
  | 'cstring'

export interface FFIFunction {
  args: FFIType[]
  returns: FFIType
}

export interface FFISymbols {
  [name: string]: FFIFunction
}

export interface FFILibrary {
  symbols: Record<string, (...args: unknown[]) => unknown>
  close(): void
}

function notAvailable(name: string): never {
  throw new Error(
    `bun:ffi ${name}() is not available in workerd. FFI requires native code execution.`,
  )
}

/**
 * Load a dynamic library
 */
export function dlopen(_path: string, _symbols: FFISymbols): FFILibrary {
  notAvailable('dlopen')
}

/**
 * Create a C string
 */
export function CString(_ptr: number): string {
  notAvailable('CString')
}

/**
 * Get a pointer to data
 */
export function ptr(_data: ArrayBuffer | Uint8Array): number {
  notAvailable('ptr')
}

/**
 * Read memory at a pointer
 */
export function read(_ptr: number, _type: FFIType): unknown {
  notAvailable('read')
}

/**
 * Write memory at a pointer
 */
export function write(_ptr: number, _value: unknown, _type: FFIType): void {
  notAvailable('write')
}

/**
 * Allocate memory
 */
export function allocate(_size: number): number {
  notAvailable('allocate')
}

/**
 * Free memory
 */
export function free(_ptr: number): void {
  notAvailable('free')
}

/**
 * Create a JSCallback for passing to native code
 */
export function callback(
  _fn: (...args: unknown[]) => unknown,
  _definition: FFIFunction,
): number {
  notAvailable('callback')
}

/**
 * Convert a pointer to an ArrayBuffer view
 */
export function toArrayBuffer(_ptr: number, _byteLength: number): ArrayBuffer {
  notAvailable('toArrayBuffer')
}

/**
 * View a native array as a TypedArray
 */
export function viewSource(
  _ptr: number,
  _type: FFIType,
  _length: number,
): ArrayBuffer {
  notAvailable('viewSource')
}

// Type constants
export const FFI_TYPES = {
  void: 0,
  bool: 1,
  char: 2,
  i8: 3,
  i16: 4,
  i32: 5,
  i64: 6,
  u8: 7,
  u16: 8,
  u32: 9,
  u64: 10,
  f32: 11,
  f64: 12,
  pointer: 13,
  cstring: 14,
} as const

export default {
  dlopen,
  CString,
  ptr,
  read,
  write,
  allocate,
  free,
  callback,
  toArrayBuffer,
  viewSource,
  FFI_TYPES,
}
