// Type guards for Bun runtime

export function isString(value: unknown): value is string {
  return typeof value === 'string'
}

export function isArrayBuffer(value: unknown): value is ArrayBuffer {
  return value instanceof ArrayBuffer
}

export function isUint8Array(value: unknown): value is Uint8Array {
  return value instanceof Uint8Array
}
