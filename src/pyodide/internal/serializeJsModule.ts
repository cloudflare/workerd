// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* eslint-disable prefer-rest-params */
/* eslint-disable @typescript-eslint/no-unsafe-member-access */
/* eslint-disable @typescript-eslint/no-unsafe-argument */
/* eslint-disable @typescript-eslint/no-unsafe-assignment */
import { IS_CREATING_SNAPSHOT } from 'pyodide-internal:metadata';

export type SerializedJsModule = {
  jsModule: true;
  moduleName: string;
  accessorList: string[];
};

const importName = Symbol('importName');
const getAccessorList = Symbol('getAccessorList');
const getObject = Symbol('getObject');
const getPrototypeOfKey = 'Reflect.getProtoTypeOf';

export function maybeSerializeJsModule(
  obj_: any,
  modules: Set<string>
): SerializedJsModule | undefined {
  const obj = obj_ as
    | { [importName]: string; [getAccessorList]: string[] }
    | undefined;
  const moduleName = obj?.[importName];
  if (!moduleName) {
    return undefined;
  }
  modules.add(moduleName);
  const accessorList: string[] = obj[getAccessorList];
  return { jsModule: true, moduleName, accessorList };
}

interface JsModules {
  [a: string]: JsModules;
}

export function deserializeJsModule(
  obj: SerializedJsModule,
  jsModules: JsModules
): unknown {
  const { accessorList, moduleName } = obj;
  const result =
    accessorList.reduce((x: JsModules, y: string): JsModules => {
      if (y === getPrototypeOfKey) {
        return Reflect.getPrototypeOf(x) as JsModules;
      }
      return x[y]!;
    }, jsModules[moduleName]!) ?? null;
  // Support stacked snapshots
  return createImportProxy(moduleName, result, accessorList);
}

// This tracks the information needed to "serialize" attributes of js modules. We need the name and
// the sequence of attribute accesses. We store the name and accessorList under the importName and
// getAccessorList symbols.
//
// If the receiver of a function call is an import proxy, this can cause the call to crash, so we
// unwrap the receiver using the getObject symbol.
export function createImportProxy<T>(
  name: string,
  mod: T,
  accessorList: (string | symbol)[] = []
): T {
  if (!IS_CREATING_SNAPSHOT) {
    return mod;
  }
  if (!mod || !['object', 'function'].includes(typeof mod)) {
    return mod;
  }
  return new Proxy(mod, {
    get(target: any, prop: string | symbol, _receiver): any {
      if (prop === importName) {
        return name;
      }
      if (prop === getAccessorList) {
        return accessorList;
      }
      if (prop === getObject) {
        return target;
      }
      // @ts-expect-error untyped Reflect.get
      const orig = Reflect.get(...arguments);
      const descr = Reflect.getOwnPropertyDescriptor(target, prop);
      // We're required to return the original value unmodified if it's an own
      // property with a non-writable, non-configurable data descriptor
      if (descr && descr.writable === false && !descr.configurable) {
        return orig;
      }
      // Or an accessor descriptor with a setter but no getter
      if (descr && descr.set && !descr.get) {
        return orig;
      }
      if (!['object', 'function'].includes(typeof orig)) {
        return orig;
      }
      return createImportProxy(name, orig, [...accessorList, prop]);
    },
    apply(target: any, thisArg: any, argumentList: any[]): any {
      // If thisArg is a GlobalsProxy it may break APIs that expect the receiver
      // to be unmodified. Unwrap any GlobalsProxy before making the call.
      thisArg = thisArg?.[getObject] ?? thisArg;
      return Reflect.apply(target, thisArg, argumentList);
    },
    getPrototypeOf(target: object): any {
      return createImportProxy(name, Reflect.getPrototypeOf(target), [
        ...accessorList,
        getPrototypeOfKey,
      ]);
    },
  }) as T;
}
