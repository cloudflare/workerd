// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import { test } from "node:test";
import path from "path";
import ts from "typescript";
import { printer } from "../../src/print";
import { createMemoryProgram } from "../../src/program";
import { createClassToInterfaceTransformer } from "../../src/transforms/class-to-interface";

test("createClassToInterfaceTransformer: transforms class to interface", () => {
  const source = `
    /**
     * MyClass
     */
    class MyClass<T = void, U = void> {
      /* constructor */
      constructor(str: string): MyClass;
      /* prop */
      prop: T;
      /* method */
      method(): U {}
      /* getter */
      get accessor(): number { return 42; }
      /* static method */
      static staticMethod(str?: string): void {}
      /* private method */
      private privateMethod() {}
    }
  `;

  const expectedOutput = `
    declare var MyClass: {
      prototype: MyClass;
      /* constructor */
      new <T = void, U = void>(str: string): MyClass<T, U>;
      /* static method */
      staticMethod(str?: string): void;
    };
    /**
     * MyClass
     */
    interface MyClass<T = void, U = void> {
      /* prop */
      prop: T;
      /* method */
      method(): U;
      /* getter */
      accessor: number;
    }
  `;

  const sourcePath = path.resolve(__dirname, "source.ts");
  const sources = new Map([[sourcePath, source]]);
  const program = createMemoryProgram(sources);
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  const result = ts.transform(sourceFile, [
    createClassToInterfaceTransformer(["MyClass"]),
  ]);
  assert.strictEqual(result.transformed.length, 1);

  const output = printer.printFile(result.transformed[0]);
  assert.strictEqual(
    normalizeWhitespace(output.trim()),
    normalizeWhitespace(expectedOutput.trim()),
    "The transformed output did not match the expected output"
  );
});

function normalizeWhitespace(str: string) {
  return str.replace(/\s+/g, " ").trim();
}
