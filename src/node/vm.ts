// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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

import {
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';

import {
  validateArray,
  validateBoolean,
  validateBuffer,
  validateInt32,
  validateOneOf,
  validateObject,
  validateString,
  validateStringArray,
  validateUint32,
  kValidateObjectAllowArray,
  kValidateObjectAllowNullable,
} from 'node-internal:validators';

import { Buffer } from 'node-internal:internal_buffer';

import type {
  CreateContextOptions,
  RunningScriptOptions,
  RunningScriptInNewContextOptions,
  ScriptOptions,
  CompileFunctionOptions,
  MeasureMemoryOptions,
  MemoryMeasurement,
} from 'node:vm';

export const constants = {
  __proto__: null,
  USE_MAIN_CONTEXT_DEFAULT_LOADER: Symbol(
    'vm_dynamic_import_main_context_default'
  ),
  DONT_CONTEXTIFY: Symbol('vm_context_no_contextify'),
};
Object.freeze(constants);

export function isContext(object: unknown): boolean {
  validateObject(object, 'object', kValidateObjectAllowArray);
  // Non-functional stub
  return false;
}

export class Script {
  cachedDataRejected: undefined | boolean = undefined;
  cachedDataProduced: undefined | boolean = undefined;
  cachedData: undefined | Buffer = undefined;
  sourceMapURL: undefined | string = undefined;

  constructor(_code: string, options: ScriptOptions | string = {}) {
    _code = `${_code}`; // eslint-disable-line @typescript-eslint/no-unnecessary-template-expression
    if (typeof options === 'string') {
      options = { filename: options };
    } else {
      validateObject(options, 'options');
    }

    const {
      filename = 'evalmachine.<anonymous>',
      lineOffset = 0,
      columnOffset = 0,
      cachedData,
      produceCachedData = false, // eslint-disable-line @typescript-eslint/no-deprecated
      importModuleDynamically = false,
    } = options;

    validateBoolean(importModuleDynamically, 'options.importModuleDynamically');
    validateString(filename, 'options.filename');
    validateInt32(lineOffset, 'options.lineOffset');
    validateInt32(columnOffset, 'options.columnOffset');
    if (cachedData !== undefined) {
      validateBuffer(cachedData, 'options.cachedData');
    }
    validateBoolean(produceCachedData, 'options.produceCachedData');

    throw new ERR_METHOD_NOT_IMPLEMENTED('Script');
  }

  createCachedData(): Buffer {
    return Buffer.alloc(0);
  }

  runInThisContext(options: RunningScriptOptions = {}): unknown {
    validateObject(options, 'options');
    const {
      breakOnSigint = false,
      displayErrors = true,
      timeout = 0,
    } = options;
    validateBoolean(breakOnSigint, 'options.breakOnSigint');
    validateBoolean(displayErrors, 'options.displayErrors');
    validateUint32(timeout, 'options.timeout', true);
    throw new ERR_METHOD_NOT_IMPLEMENTED('runInThisContext');
  }

  runInContext(
    contextifiedObject: object,
    options: RunningScriptOptions = {}
  ): unknown {
    validateContext(contextifiedObject);
    validateObject(options, 'options');
    const {
      breakOnSigint = false,
      displayErrors = true,
      timeout = 0,
    } = options;
    validateBoolean(breakOnSigint, 'options.breakOnSigint');
    validateBoolean(displayErrors, 'options.displayErrors');
    validateUint32(timeout, 'options.timeout', true);
    throw new ERR_METHOD_NOT_IMPLEMENTED('runInThisContext');
  }

  runInNewContext(
    contextObject: object | symbol,
    options: RunningScriptInNewContextOptions = {}
  ): unknown {
    const context = createContext(contextObject, getContextOptions(options));
    return this.runInContext(context, options);
  }
}

function validateContext(contextifiedObject: object): void {
  if (!isContext(contextifiedObject)) {
    throw new ERR_INVALID_ARG_TYPE(
      'contextifiedObject',
      'vm.Context',
      contextifiedObject
    );
  }
}

function getContextOptions(
  options?: RunningScriptInNewContextOptions
): CreateContextOptions {
  if (!options) return {};
  const contextOptions: CreateContextOptions = {
    name: options.contextName,
    origin: options.contextOrigin,
    codeGeneration: undefined,
    microtaskMode: options.microtaskMode,
  };
  if (contextOptions.name !== undefined)
    validateString(contextOptions.name, 'options.contextName');
  if (contextOptions.origin !== undefined)
    validateString(contextOptions.origin, 'options.contextOrigin');
  if (options.contextCodeGeneration !== undefined) {
    validateObject(
      options.contextCodeGeneration,
      'options.contextCodeGeneration'
    );
    const { strings, wasm } = options.contextCodeGeneration;
    if (strings !== undefined)
      validateBoolean(strings, 'options.contextCodeGeneration.strings');
    if (wasm !== undefined)
      validateBoolean(wasm, 'options.contextCodeGeneration.wasm');
    contextOptions.codeGeneration = { strings, wasm };
  }
  if (options.microtaskMode !== undefined)
    validateString(options.microtaskMode, 'options.microtaskMode');
  return contextOptions;
}

export function createContext(
  contextObject = {},
  options: CreateContextOptions = {}
): object {
  if (contextObject !== constants.DONT_CONTEXTIFY && isContext(contextObject)) {
    return contextObject;
  }

  validateObject(options, 'options');

  const {
    name = `VM Context`,
    origin,
    codeGeneration,
    microtaskMode,
    importModuleDynamically = false,
  } = options;

  validateString(name, 'options.name');
  if (origin !== undefined) validateString(origin, 'options.origin');
  if (codeGeneration !== undefined)
    validateObject(codeGeneration, 'options.codeGeneration');
  validateBoolean(importModuleDynamically, 'options.importModuleDynamically');

  let strings = true;
  let wasm = true;
  if (codeGeneration !== undefined) {
    ({ strings = true, wasm = true } = codeGeneration as {
      strings?: boolean | undefined;
      wasm?: boolean | undefined;
    });
    validateBoolean(strings, 'options.codeGeneration.strings');
    validateBoolean(wasm, 'options.codeGeneration.wasm');
  }

  validateOneOf(microtaskMode, 'options.microtaskMode', [
    'afterEvaluate',
    undefined,
  ]);
  throw new ERR_METHOD_NOT_IMPLEMENTED('createContext');
}

export function createScript(
  code: string,
  options: ScriptOptions = {}
): Script {
  return new Script(code, options);
}

export function runInContext(
  code: string,
  contextifiedObject: object,
  options: RunningScriptOptions = {}
): unknown {
  validateContext(contextifiedObject);
  if (typeof options === 'string') {
    options = {
      filename: options,
    };
  }
  return createScript(code, options).runInContext(contextifiedObject, options);
}

export function runInNewContext(
  code: string,
  contextObject: object | RunningScriptInNewContextOptions | symbol,
  options?: RunningScriptInNewContextOptions | string
): unknown {
  if (typeof options === 'string') {
    options = { filename: options };
  } else {
    options = { ...options };
  }
  contextObject = createContext(contextObject, getContextOptions(options));
  return createScript(code, options).runInNewContext(contextObject, options);
}

export function runInThisContext(
  code: string,
  options: RunningScriptOptions | string = {}
): unknown {
  if (typeof options === 'string') {
    options = { filename: options };
  }
  return createScript(code, options).runInThisContext(options);
}

export function compileFunction(
  code: string,
  params: readonly string[] = [],
  options: CompileFunctionOptions = {}
  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
): Function & {
  cachedData?: Script['cachedData'] | undefined;
  cachedDataProduced?: Script['cachedDataProduced'] | undefined;
  cachedDataRejected?: Script['cachedDataRejected'] | undefined;
} {
  validateString(code, 'code');
  validateObject(options, 'options');
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (params !== undefined) {
    validateStringArray(params, 'params');
  }
  const {
    filename = '',
    columnOffset = 0,
    lineOffset = 0,
    cachedData = undefined,
    produceCachedData = false, // eslint-disable-line @typescript-eslint/no-deprecated
    parsingContext = undefined,
    contextExtensions = [],
    importModuleDynamically = false,
  } = options;

  validateString(filename, 'options.filename');
  validateInt32(columnOffset, 'options.columnOffset');
  validateInt32(lineOffset, 'options.lineOffset');
  if (cachedData !== undefined)
    validateBuffer(cachedData, 'options.cachedData');
  validateBoolean(produceCachedData, 'options.produceCachedData');
  validateBoolean(importModuleDynamically, 'options.importModuleDynamically');
  if (parsingContext !== undefined) {
    if (
      typeof parsingContext !== 'object' ||
      parsingContext === null || // eslint-disable-line @typescript-eslint/no-unnecessary-condition
      !isContext(parsingContext)
    ) {
      throw new ERR_INVALID_ARG_TYPE(
        'options.parsingContext',
        'Context',
        parsingContext
      );
    }
  }
  validateArray(contextExtensions, 'options.contextExtensions');
  contextExtensions.forEach((extension, i) => {
    const name = `options.contextExtensions[${i}]`;
    validateObject(extension, name, kValidateObjectAllowNullable);
  });

  throw new ERR_METHOD_NOT_IMPLEMENTED('compileFunction');
}

export function measureMemory(
  options: MeasureMemoryOptions = {}
): Promise<MemoryMeasurement> {
  validateObject(options, 'options');
  const { mode = 'summary', execution = 'default' } = options;
  validateOneOf(mode, 'options.mode', ['summary', 'detailed']);
  validateOneOf(execution, 'options.execution', ['default', 'eager']);
  throw new ERR_METHOD_NOT_IMPLEMENTED('measureMemory');
}

export default {
  Script,
  createContext,
  createScript,
  runInContext,
  runInNewContext,
  runInThisContext,
  isContext,
  compileFunction,
  measureMemory,
  constants,
  // TODO(later): They are still experimental, but eventually we should have non-op
  // exports for vm.Module, vm.SourceTextModule, and vm.SyntheticModule
};
