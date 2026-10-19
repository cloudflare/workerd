// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface VmContext {
  getGlobal(): object;
}
export interface VmCallbacks {
  initializeImportMeta?: (meta: Record<string, unknown>) => void;
  importModuleDynamically?: (
    specifier: string,
    attributes: Record<string, string>
  ) => Promise<object>;
  evaluate?: () => void;
}
export interface VmScript {
  run(context?: VmContext): unknown;
  createCachedData(): ArrayBuffer;
}
export interface ModuleRequest {
  specifier: string;
  attributes: Record<string, string>;
}
export interface VmModule {
  getRequests(): ModuleRequest[];
  setDependencies(dependencies: VmModule[]): void;
  instantiate(): void;
  evaluate(): Promise<unknown>;
  getNamespace(): object;
  getError(): unknown;
  getStatus(): 0 | 1 | 2 | 3 | 4 | 5;
  setExport(name: string, value: unknown): void;
}
export interface VmUtil {
  createContext(
    sandbox: object | undefined,
    strings: boolean,
    wasm: boolean
  ): VmContext;
  mainContext(): VmContext;
  compileScript(
    code: string,
    filename: string,
    lineOffset: number,
    columnOffset: number,
    callbacks: VmCallbacks
  ): VmScript;
  compileFunction(
    code: string,
    params: readonly string[],
    filename: string,
    lineOffset: number,
    columnOffset: number,
    context: VmContext | undefined,
    extensions: object[],
    callbacks: VmCallbacks
  ): (...args: unknown[]) => unknown;
  sourceTextModule(
    context: VmContext,
    code: string,
    identifier: string,
    lineOffset: number,
    columnOffset: number,
    callbacks: VmCallbacks
  ): VmModule;
  syntheticModule(
    context: VmContext,
    exports: string[],
    identifier: string,
    callbacks: VmCallbacks
  ): VmModule;
}

declare const moduleDefault: VmUtil;
export default moduleDefault;
