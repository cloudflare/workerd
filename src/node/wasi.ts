// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { WASIOptions } from 'node:wasi'

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors'
import {
  validateArray,
  validateBoolean,
  validateInt32,
  validateObject,
  validateOneOf,
} from 'node-internal:validators'

interface FinalizeBindingsOptions {
  memory?: WebAssembly.Memory | undefined
}

// TODO(later): This is one we actually might want to implement at some point.

export class WASI {
  constructor(options: WASIOptions = { version: 'unstable' }) {
    validateObject(options, 'options')
    validateOneOf(options.version, 'options.version', ['unstable', 'preview1'])
    if (options.args !== undefined) {
      validateArray(options.args, 'options.args')
    }
    if (options.env !== undefined) {
      validateObject(options.env, 'options.env')
    }
    if (options.preopens !== undefined) {
      validateObject(options.preopens, 'options.preopens')
    }
    const { stdin = 0, stdout = 1, stderr = 2 } = options
    validateInt32(stdin, 'options.stdin', 0)
    validateInt32(stdout, 'options.stdout', 0)
    validateInt32(stderr, 'options.stderr', 0)

    if (options.returnOnExit !== undefined) {
      validateBoolean(options.returnOnExit, 'options.returnOnExit')
    }

    throw new ERR_METHOD_NOT_IMPLEMENTED('WASI')
  }

  finalizeBindings(_1: object, _2: FinalizeBindingsOptions = {}): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('WASI.finalizeBindings')
  }

  start(_: object): number {
    throw new ERR_METHOD_NOT_IMPLEMENTED('WASI.start')
  }

  initialize(_: object): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('WASI.initialize')
  }

  getImportObject(): object {
    return {}
  }
}

export default { WASI }
