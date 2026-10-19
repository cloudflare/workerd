// Copyright (c) 2026 Cloudflare, Inc.
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

import nativeVm from 'node-internal:vm';
import type * as NativeVm from 'node-internal:vm';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_METHOD_NOT_IMPLEMENTED,
  NodeError,
  NodeTypeError,
} from 'node-internal:internal_errors';
import {
  validateArray,
  validateBoolean,
  validateBuffer,
  validateFunction,
  validateInt32,
  validateObject,
  validateString,
  validateStringArray,
  validateUint32,
} from 'node-internal:validators';
import { isModuleNamespaceObject } from 'node-internal:internal_types';

export const realVmEnabled =
  !!Cloudflare.compatibilityFlags['enable_nodejs_vm_context'];
export const contexts = new WeakMap<object, NativeVm.VmContext>();
const contextIds = new WeakMap<object, number>();
let nextId = 0;
const noError = Symbol('no error');
const statuses = [
  'unlinked',
  'linking',
  'linked',
  'evaluating',
  'evaluated',
  'errored',
] as const;

type Status = (typeof statuses)[number];
type Linker = (
  specifier: string,
  referencingModule: Module,
  extra: {
    attributes: Record<string, string>;
    assert: Record<string, string>;
  }
) => Module | Promise<Module>;
interface ModuleOptions {
  context?: object;
  identifier?: string;
}
interface SourceOptions extends ModuleOptions {
  lineOffset?: number;
  columnOffset?: number;
  cachedData?: Uint8Array;
  initializeImportMeta?: (
    meta: Record<string, unknown>,
    module: SourceTextModule
  ) => void;
  importModuleDynamically?: (
    specifier: string,
    module: SourceTextModule,
    attributes: Record<string, string>,
    phase: string
  ) => unknown;
}

function statusError(message: string): NodeError {
  return new NodeError('ERR_VM_MODULE_STATUS', `Module status ${message}`);
}

export function validateRunOptions(options: {
  timeout?: number | undefined;
  breakOnSigint?: boolean | undefined;
}): void {
  validateObject(options, 'options');
  if (options.timeout !== undefined) {
    validateUint32(options.timeout, 'options.timeout', true);
    throw new ERR_METHOD_NOT_IMPLEMENTED('options.timeout');
  }
  if (options.breakOnSigint !== undefined)
    validateBoolean(options.breakOnSigint, 'options.breakOnSigint');
  if (options.breakOnSigint)
    throw new ERR_METHOD_NOT_IMPLEMENTED('options.breakOnSigint');
}

export function dynamicImportCallback(
  callback: unknown,
  referrer: () => unknown
): NonNullable<NativeVm.VmCallbacks['importModuleDynamically']> {
  if (callback !== undefined) {
    if (typeof callback === 'symbol')
      throw new ERR_METHOD_NOT_IMPLEMENTED('USE_MAIN_CONTEXT_DEFAULT_LOADER');
    validateFunction(callback, 'options.importModuleDynamically');
  }
  return async (specifier, attributes) => {
    if (callback === undefined) {
      throw new NodeTypeError(
        'ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING',
        'A dynamic import callback was not specified.'
      );
    }
    const result: unknown = await Reflect.apply(callback, undefined, [
      specifier,
      referrer(),
      attributes,
      'evaluation',
    ]);
    if (isModuleNamespaceObject(result)) return result as object;
    if (!isModule(result))
      throw new NodeError(
        'ERR_VM_MODULE_NOT_MODULE',
        'Provided module is not an instance of Module'
      );
    if (result.status === 'errored') throw result.error;
    return result.namespace;
  };
}

let initializeModule: (
  module: Module,
  create: (context: NativeVm.VmContext, identifier: string) => NativeVm.VmModule
) => void;
let moduleHandle: (module: Module) => NativeVm.VmModule;
let isModule: (value: unknown) => value is Module;

export class Module {
  #handle!: NativeVm.VmModule;
  #identifier: string;
  #context: object | undefined;
  #linking = false;
  #linkError: unknown = noError;

  constructor(options: ModuleOptions = {}) {
    if (new.target === Module)
      throw new TypeError('Module is not a constructor');
    if (!realVmEnabled) throw new ERR_METHOD_NOT_IMPLEMENTED('Module');
    validateObject(options, 'options');
    const { context, identifier } = options;
    if (context !== undefined && !contexts.has(context))
      throw new ERR_INVALID_ARG_TYPE('options.context', 'vm.Context', context);
    if (identifier !== undefined)
      validateString(identifier, 'options.identifier');
    this.#context = context;
    if (identifier === undefined) {
      const id =
        context === undefined ? nextId++ : (contextIds.get(context) ?? 0);
      if (context !== undefined) contextIds.set(context, id + 1);
      this.#identifier = `vm:module(${id})`;
    } else {
      this.#identifier = identifier;
    }
  }

  // Only the two public constructors may install native handles; no handle is exposed to callers.
  static {
    initializeModule = (module, create): void => {
      const context =
        module.#context === undefined
          ? nativeVm.mainContext()
          : contexts.get(module.#context);
      if (context === undefined) {
        throw new ERR_INVALID_ARG_TYPE(
          'options.context',
          'vm.Context',
          module.#context
        );
      }
      module.#handle = create(context, module.#identifier);
    };
    moduleHandle = (module): NativeVm.VmModule => module.#handle;
    isModule = (value): value is Module =>
      typeof value === 'object' && value !== null && #handle in value;
  }

  get identifier(): string {
    return this.#identifier;
  }
  get context(): object | undefined {
    return this.#context;
  }
  get status(): Status {
    if (this.#linkError !== noError) return 'errored';
    if (this.#linking) return 'linking';
    return statuses[this.#handle.getStatus()];
  }
  get namespace(): object {
    if (this.#handle.getStatus() < 2)
      throw statusError('must not be unlinked or linking');
    return this.#handle.getNamespace();
  }
  get error(): unknown {
    if (this.#linkError !== noError) return this.#linkError;
    if (this.#handle.getStatus() !== 5) throw statusError('must be errored');
    return this.#handle.getError();
  }

  link(linker: Linker): Promise<void> | void {
    return this.#linkAndInstantiate(linker);
  }

  async #linkAndInstantiate(linker: Linker): Promise<void> {
    validateFunction(linker, 'linker');
    if (this.status === 'linked')
      throw new NodeError(
        'ERR_VM_MODULE_ALREADY_LINKED',
        'Module has already been linked'
      );
    if (this.status !== 'unlinked') throw statusError('must be unlinked');
    await this.#link(linker);
    this.#handle.instantiate();
  }

  async #link(linker: Linker): Promise<void> {
    this.#linking = true;
    // Node invokes linkers synchronously in request order. A synchronous throw
    // stops later callbacks and leaves this module in the linking state.
    const dependencies = this.#handle
      .getRequests()
      .map(({ specifier, attributes }) => {
        const result = linker(specifier, this, {
          attributes,
          assert: attributes,
        });
        return Promise.resolve(result).then(async (dependency) => {
          if (!isModule(dependency))
            throw new NodeError(
              'ERR_VM_MODULE_NOT_MODULE',
              'Provided module is not an instance of Module'
            );
          if (dependency.context !== this.context)
            throw new NodeError(
              'ERR_VM_MODULE_DIFFERENT_CONTEXT',
              'Linked modules must use the same context'
            );
          if (dependency.status === 'errored') {
            const error = new NodeError(
              'ERR_VM_MODULE_LINK_FAILURE',
              `request for '${specifier}' resolved to an errored module`
            );
            error.cause = dependency.error;
            throw error;
          }
          // An ancestor is already linking in a cycle. Its native dependencies will
          // be installed before the root calls instantiate().
          if (dependency.status === 'unlinked') await dependency.#link(linker);
          return dependency.#handle;
        });
      });
    try {
      this.#handle.setDependencies(await Promise.all(dependencies));
    } catch (error) {
      this.#linkError = error;
      throw error;
    } finally {
      this.#linking = false;
    }
  }

  async evaluate(
    options: { timeout?: number | undefined; breakOnSigint?: boolean } = {}
  ): Promise<void> {
    validateRunOptions(options);
    const status = this.#handle.getStatus();
    if (
      this.#linkError !== noError ||
      (status !== 2 && status !== 4 && status !== 5)
    )
      throw statusError('must be one of linked, evaluated, or errored');
    await this.#handle.evaluate();
  }
}

export class SourceTextModule extends Module {
  #requests: readonly Readonly<
    NativeVm.ModuleRequest & { phase: 'evaluation' }
  >[];
  #specifiers: readonly string[];

  constructor(sourceText: string, options: SourceOptions = {}) {
    validateString(sourceText, 'sourceText');
    validateObject(options, 'options');
    const {
      lineOffset = 0,
      columnOffset = 0,
      cachedData,
      initializeImportMeta,
      importModuleDynamically,
    } = options;
    validateInt32(lineOffset, 'options.lineOffset');
    validateInt32(columnOffset, 'options.columnOffset');
    if (initializeImportMeta !== undefined)
      validateFunction(initializeImportMeta, 'options.initializeImportMeta');
    if (importModuleDynamically !== undefined)
      validateFunction(
        importModuleDynamically,
        'options.importModuleDynamically'
      );
    if (cachedData !== undefined) {
      validateBuffer(cachedData, 'options.cachedData');
      throw new ERR_METHOD_NOT_IMPLEMENTED('SourceTextModule cachedData');
    }
    super(options);
    initializeModule(this, (context, identifier) =>
      nativeVm.sourceTextModule(
        context,
        sourceText,
        identifier,
        lineOffset,
        columnOffset,
        {
          initializeImportMeta: (meta) => {
            initializeImportMeta?.(meta, this);
          },
          importModuleDynamically: dynamicImportCallback(
            importModuleDynamically,
            () => this
          ),
        }
      )
    );
    this.#requests = Object.freeze(
      moduleHandle(this)
        .getRequests()
        .map((request) =>
          Object.freeze({ ...request, phase: 'evaluation' as const })
        )
    );
    this.#specifiers = Object.freeze(
      this.#requests.map(({ specifier }) => specifier)
    );
  }

  get moduleRequests(): readonly Readonly<
    NativeVm.ModuleRequest & { phase: 'evaluation' }
  >[] {
    return this.#requests;
  }
  get dependencySpecifiers(): readonly string[] {
    return this.#specifiers;
  }
  linkRequests(modules: Module[]): void {
    if (this.status !== 'unlinked') throw statusError('must be unlinked');
    validateArray(modules, 'modules');
    if (modules.length !== this.#requests.length)
      throw new NodeTypeError(
        'ERR_MODULE_LINK_MISMATCH',
        `Expected ${this.#requests.length} modules, got ${modules.length}`
      );
    const handles = modules.map((module) => {
      if (!isModule(module))
        throw new NodeError(
          'ERR_VM_MODULE_NOT_MODULE',
          'Provided module is not an instance of Module'
        );
      if (module.context !== this.context)
        throw new NodeError(
          'ERR_VM_MODULE_DIFFERENT_CONTEXT',
          'Linked modules must use the same context'
        );
      return moduleHandle(module);
    });
    moduleHandle(this).setDependencies(handles);
  }
  instantiate(): void {
    if (this.status !== 'unlinked') throw statusError('must be unlinked');
    moduleHandle(this).instantiate();
  }
  createCachedData(): never {
    throw new ERR_METHOD_NOT_IMPLEMENTED('SourceTextModule.createCachedData');
  }
}

export class SyntheticModule extends Module {
  #exportNames: Set<string>;

  constructor(
    exportNames: string[],
    evaluateCallback: (this: SyntheticModule) => void,
    options: ModuleOptions = {}
  ) {
    validateStringArray(exportNames, 'exportNames');
    const names = new Set<string>();
    for (const name of exportNames) {
      if (names.has(name))
        throw new ERR_INVALID_ARG_VALUE(
          `exportNames.${name}`,
          name,
          'is duplicated'
        );
      names.add(name);
    }
    validateFunction(evaluateCallback, 'evaluateCallback');
    super(options);
    this.#exportNames = names;
    initializeModule(this, (context, identifier) =>
      nativeVm.syntheticModule(context, exportNames, identifier, {
        evaluate: () => {
          Reflect.apply(evaluateCallback, this, []);
        },
      })
    );
    moduleHandle(this).instantiate();
  }

  override link(_linker?: Linker): void {}

  setExport(name: string, value: unknown): void {
    validateString(name, 'name');
    if (!this.#exportNames.has(name))
      throw new ReferenceError(`Export '${name}' is not defined in module`);
    moduleHandle(this).setExport(name, value);
  }
}
