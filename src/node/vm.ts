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

import nativeVm from 'node-internal:vm';
import type * as NativeVm from 'node-internal:vm';
import {
  Module as VmModule,
  SourceTextModule as VmSourceTextModule,
  SyntheticModule as VmSyntheticModule,
  contexts,
  realVmEnabled,
  dynamicImportCallback,
  validateRunOptions,
} from 'node-internal:internal_vm_module';
export const Module = realVmEnabled ? VmModule : undefined;
export const SourceTextModule = realVmEnabled ? VmSourceTextModule : undefined;
export const SyntheticModule = realVmEnabled ? VmSyntheticModule : undefined;

export const constants = {
  __proto__: null,
  USE_MAIN_CONTEXT_DEFAULT_LOADER: Symbol(
    'vm_dynamic_import_main_context_default'
  ),
  DONT_CONTEXTIFY: Symbol('vm_context_no_contextify'),
};
Object.freeze(constants);

const kContextSymbol = Symbol('vm_context_symbol');

export function isContext(object: unknown): object is object {
  validateObject(object, 'object', kValidateObjectAllowArray);
  return realVmEnabled
    ? contexts.has(object)
    : kContextSymbol in object && object[kContextSymbol] === true;
}

export class Script {
  #script: NativeVm.VmScript | undefined;
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

    if (!realVmEnabled)
      validateBoolean(
        importModuleDynamically,
        'options.importModuleDynamically'
      );
    validateString(filename, 'options.filename');
    validateInt32(lineOffset, 'options.lineOffset');
    validateInt32(columnOffset, 'options.columnOffset');
    if (cachedData !== undefined) {
      validateBuffer(cachedData, 'options.cachedData');
    }
    validateBoolean(produceCachedData, 'options.produceCachedData');
    if (realVmEnabled) {
      if (cachedData !== undefined)
        throw new ERR_METHOD_NOT_IMPLEMENTED('options.cachedData');
      this.#script = nativeVm.compileScript(
        _code,
        filename,
        lineOffset,
        columnOffset,
        {
          importModuleDynamically: dynamicImportCallback(
            options.importModuleDynamically,
            () => this
          ),
        }
      );
      if (produceCachedData) {
        this.cachedData = this.createCachedData();
        this.cachedDataProduced = true;
      }
    }
  }

  createCachedData(): Buffer {
    return this.#script === undefined
      ? Buffer.alloc(0)
      : Buffer.from(this.#script.createCachedData());
  }

  runInThisContext(options: RunningScriptOptions = {}): unknown {
    validateObject(options, 'options');
    const { breakOnSigint = false, displayErrors = true, timeout } = options;
    validateBoolean(breakOnSigint, 'options.breakOnSigint');
    validateBoolean(displayErrors, 'options.displayErrors');
    if (timeout !== undefined) {
      validateUint32(timeout, 'options.timeout', true);
    }
    if (this.#script !== undefined) {
      validateRunOptions(options);
      return this.#script.run();
    }
    throw new ERR_METHOD_NOT_IMPLEMENTED('runInThisContext');
  }

  runInContext(
    contextifiedObject: object,
    options: RunningScriptOptions = {}
  ): unknown {
    validateContext(contextifiedObject);
    validateObject(options, 'options');
    const { breakOnSigint = false, displayErrors = true, timeout } = options;
    validateBoolean(breakOnSigint, 'options.breakOnSigint');
    validateBoolean(displayErrors, 'options.displayErrors');
    if (timeout !== undefined) {
      validateUint32(timeout, 'options.timeout', true);
    }
    if (this.#script !== undefined) {
      validateRunOptions(options);
      return this.#script.run(contexts.get(contextifiedObject));
    }
    throw new ERR_METHOD_NOT_IMPLEMENTED('runInThisContext');
  }

  runInNewContext(
    contextObject: object | symbol = {},
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
  contextObject: object | symbol = {},
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
  if (!realVmEnabled)
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

  if (realVmEnabled) {
    if (microtaskMode !== undefined)
      throw new ERR_METHOD_NOT_IMPLEMENTED('options.microtaskMode');
    if (options.importModuleDynamically !== undefined)
      throw new ERR_METHOD_NOT_IMPLEMENTED('options.importModuleDynamically');
    let sandbox: object | undefined;
    if (contextObject !== constants.DONT_CONTEXTIFY) {
      validateObject(contextObject, 'contextObject', kValidateObjectAllowArray);
      sandbox = contextObject;
    }
    const context = nativeVm.createContext(sandbox, strings, wasm);
    const result = sandbox ?? context.getGlobal();
    contexts.set(result, context);
    return result;
  }

  return Object.create(null, {
    [kContextSymbol]: {
      value: true,
    },
  }) as object;
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
  contextObject: object | RunningScriptInNewContextOptions | symbol = {},
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
    cachedData,
    produceCachedData = false, // eslint-disable-line @typescript-eslint/no-deprecated
    parsingContext,
    contextExtensions = [],
    importModuleDynamically = false,
  } = options;

  validateString(filename, 'options.filename');
  validateInt32(columnOffset, 'options.columnOffset');
  validateInt32(lineOffset, 'options.lineOffset');
  if (cachedData !== undefined)
    validateBuffer(cachedData, 'options.cachedData');
  validateBoolean(produceCachedData, 'options.produceCachedData');
  if (!realVmEnabled)
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
  const extensions: object[] = [];
  contextExtensions.forEach((extension: unknown, i) => {
    if (extension === null) return;
    const name = `options.contextExtensions[${i}]`;
    validateObject(extension, name);
    extensions.push(extension);
  });

  if (realVmEnabled) {
    if (cachedData !== undefined || produceCachedData)
      throw new ERR_METHOD_NOT_IMPLEMENTED('compileFunction cachedData');
    const callbacks = {
      importModuleDynamically: dynamicImportCallback(
        options.importModuleDynamically,
        (): unknown => fn
      ),
    };
    const fn = nativeVm.compileFunction(
      code,
      params,
      filename,
      lineOffset,
      columnOffset,
      parsingContext === undefined ? undefined : contexts.get(parsingContext),
      extensions,
      callbacks
    );
    return fn;
  }
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
  ...(realVmEnabled ? { Module, SourceTextModule, SyntheticModule } : {}),
};
