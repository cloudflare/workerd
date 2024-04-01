// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import { test } from "node:test";
import path from "path";
import ts from "typescript";
import { printer } from "../../src/print";
import { createMemoryProgram } from "../../src/program";
import { createGlobalScopeTransformer } from "../../src/transforms";

test("createGlobalScopeTransformer: extracts global scope", () => {
  const source = `type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
};
declare class EventTarget<EventMap extends Record<string, Event> = Record<string, Event>> {
    constructor();
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void; // MethodDeclaration
    removeEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void; // MethodDeclaration
    dispatchEvent(event: EventMap[keyof EventMap]): void; // MethodDeclaration
}
declare class WorkerGlobalScope extends EventTarget<WorkerGlobalScopeEventMap> {
    thing: string; // PropertyDeclaration
    static readonly CONSTANT: 42; // PropertyDeclaration
    get property(): number; // GetAccessorDeclaration
    set property(value: number); // GetAccessorDeclaration
}
declare class DOMException {
}
declare abstract class Crypto {
}
declare abstract class Console {
}
interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
    DOMException: typeof DOMException; // PropertySignature
    btoa(value: string): string; // MethodSignature
    crypto: Crypto; // PropertySignature
    get console(): Console; // GetAccessorDeclaration
}
`;

  const sourcePath = path.resolve(__dirname, "source.ts");
  const sources = new Map([[sourcePath, source]]);
  const program = createMemoryProgram(sources);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  const result = ts.transform(sourceFile, [
    createGlobalScopeTransformer(checker),
  ]);
  assert.strictEqual(result.transformed.length, 1);

  const output = printer.printFile(result.transformed[0]);
  assert.strictEqual(
    output,
    // Extracted global nodes inserted after ServiceWorkerGlobalScope
    source +
      `declare function addEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
declare function removeEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
declare function dispatchEvent(event: WorkerGlobalScopeEventMap[keyof WorkerGlobalScopeEventMap]): void;
declare const thing: string;
declare const CONSTANT: 42;
declare const property: number;
declare function btoa(value: string): string;
declare const crypto: Crypto;
declare const console: Console;
`
  );
});

test("createGlobalScopeTransformer: inlining type parameters in heritage", () => {
  const source = `declare class A<T> {
    thing: T;
}
declare class B<T> extends A<T> {
}
declare class ServiceWorkerGlobalScope extends B<string> {
}
`;

  const sourcePath = path.resolve(__dirname, "source.ts");
  const sources = new Map([[sourcePath, source]]);
  const program = createMemoryProgram(sources);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  const result = ts.transform(sourceFile, [
    createGlobalScopeTransformer(checker),
  ]);
  assert.strictEqual(result.transformed.length, 1);

  const output = printer.printFile(result.transformed[0]);
  assert.strictEqual(
    output,
    source +
      `declare const thing: string;
`
  );
});
