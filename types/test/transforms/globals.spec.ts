import assert from "assert";
import { test } from "node:test";
import path from "path";
import ts from "typescript";
import { printer } from "../../src/print";
import { createMemoryProgram } from "../../src/program";
import { createGlobalScopeTransformer } from "../../src/transforms";

test("createGlobalScopeTransformer: extracts global scope", () => {
  const source = `export type WorkerGlobalScopeEventMap = {
    fetch: Event;
    scheduled: Event;
};
export declare class EventTarget<EventMap extends Record<string, Event> = Record<string, Event>> {
    constructor();
    addEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void; // MethodDeclaration
    removeEventListener<Type extends keyof EventMap>(type: Type, handler: (event: EventMap[Type]) => void): void; // MethodDeclaration
    dispatchEvent(event: EventMap[keyof EventMap]): void; // MethodDeclaration
}
export declare class WorkerGlobalScope extends EventTarget<WorkerGlobalScopeEventMap> {
    thing: string; // PropertyDeclaration
    static readonly CONSTANT: 42; // PropertyDeclaration
    get property(): number; // GetAccessorDeclaration
    set property(value: number); // GetAccessorDeclaration
}
export declare class DOMException {
}
export declare abstract class Crypto {
}
export declare abstract class Console {
}
export interface ServiceWorkerGlobalScope extends WorkerGlobalScope {
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
      `export declare function addEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
export declare function removeEventListener<Type extends keyof WorkerGlobalScopeEventMap>(type: Type, handler: (event: WorkerGlobalScopeEventMap[Type]) => void): void;
export declare function dispatchEvent(event: WorkerGlobalScopeEventMap[keyof WorkerGlobalScopeEventMap]): void;
export declare const thing: string;
export declare const CONSTANT: 42;
export declare const property: number;
export declare function btoa(value: string): string;
export declare const crypto: Crypto;
export declare const console: Console;
`
  );
});

test("createGlobalScopeTransformer: inlining type parameters in heritage", () => {
  const source = `export declare class A<T> {
    thing: T;
}
export declare class B<T> extends A<T> {
}
export declare class ServiceWorkerGlobalScope extends B<string> {
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
      `export declare const thing: string;
`
  );
});
