import assert from "assert";
import { test } from "node:test";
import ts from "typescript";
import { printer } from "../../src/print";
import { createMemoryProgram } from "../../src/program";
import { createGlobalScopeTransformer } from "../../src/transforms";

test("createGlobalScopeTransformer: extracts global scope", () => {
  // TODO(soon): make EventTarget generic once overrides implemented
  const source = `export declare class EventTarget {
    constructor();
    addEventListener(type: string, handler: (event: Event) => void): void; // MethodDeclaration
    removeEventListener(type: string, handler: (event: Event) => void): void; // MethodDeclaration
    dispatchEvent(event: Event): void; // MethodDeclaration
}
export declare class WorkerGlobalScope extends EventTarget {
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
  const [program, sourcePath] = createMemoryProgram(source);
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
      `export declare function addEventListener(type: string, handler: (event: Event) => void): void;
export declare function removeEventListener(type: string, handler: (event: Event) => void): void;
export declare function dispatchEvent(event: Event): void;
export declare const thing: string;
export declare const CONSTANT: 42;
export declare const property: number;
export declare function btoa(value: string): string;
export declare const crypto: Crypto;
export declare const console: Console;
`
  );
});
