// Copyright (c) 2024 Jeju Network
// Bun Internal Errors
// Licensed under the Apache 2.0 license

/**
 * Internal error classes for Bun runtime
 */

export class BunError extends Error {
  code: string

  constructor(message: string, code: string) {
    super(message)
    this.name = 'BunError'
    this.code = code
  }
}

export class ERR_NOT_IMPLEMENTED extends BunError {
  constructor(feature: string) {
    super(`${feature} is not implemented in workerd`, 'ERR_NOT_IMPLEMENTED')
    this.name = 'ERR_NOT_IMPLEMENTED'
  }
}

export class ERR_INVALID_ARG_TYPE extends BunError {
  constructor(name: string, expected: string, actual: unknown) {
    const actualType = actual === null ? 'null' : typeof actual
    super(
      `The "${name}" argument must be of type ${expected}. Received ${actualType}`,
      'ERR_INVALID_ARG_TYPE',
    )
    this.name = 'ERR_INVALID_ARG_TYPE'
  }
}

export class ERR_INVALID_ARG_VALUE extends BunError {
  constructor(name: string, value: unknown, reason?: string) {
    const msg = reason
      ? `The argument '${name}' ${reason}. Received ${String(value)}`
      : `The argument '${name}' is invalid. Received ${String(value)}`
    super(msg, 'ERR_INVALID_ARG_VALUE')
    this.name = 'ERR_INVALID_ARG_VALUE'
  }
}

export class ERR_OUT_OF_RANGE extends BunError {
  constructor(name: string, range: string, received: unknown) {
    super(
      `The value of "${name}" is out of range. It must be ${range}. Received ${received}`,
      'ERR_OUT_OF_RANGE',
    )
    this.name = 'ERR_OUT_OF_RANGE'
  }
}

export class ERR_FS_FILE_NOT_FOUND extends BunError {
  constructor(path: string) {
    super(`ENOENT: no such file or directory, open '${path}'`, 'ENOENT')
    this.name = 'ERR_FS_FILE_NOT_FOUND'
  }
}

export class ERR_FS_ACCESS_DENIED extends BunError {
  constructor(path: string, operation: string) {
    super(`EACCES: permission denied, ${operation} '${path}'`, 'EACCES')
    this.name = 'ERR_FS_ACCESS_DENIED'
  }
}

export class ERR_SQLITE_ERROR extends BunError {
  constructor(message: string) {
    super(`SQLite error: ${message}`, 'ERR_SQLITE_ERROR')
    this.name = 'ERR_SQLITE_ERROR'
  }
}

export class ERR_METHOD_NOT_IMPLEMENTED extends BunError {
  constructor(method: string, context?: string) {
    const msg = context
      ? `${method} is not implemented in ${context}`
      : `${method} is not implemented in workerd`
    super(msg, 'ERR_METHOD_NOT_IMPLEMENTED')
    this.name = 'ERR_METHOD_NOT_IMPLEMENTED'
  }
}

export class ERR_WORKERD_UNAVAILABLE extends BunError {
  constructor(feature: string, reason?: string) {
    const msg = reason
      ? `${feature} is not available in workerd: ${reason}`
      : `${feature} is not available in workerd`
    super(msg, 'ERR_WORKERD_UNAVAILABLE')
    this.name = 'ERR_WORKERD_UNAVAILABLE'
  }
}
