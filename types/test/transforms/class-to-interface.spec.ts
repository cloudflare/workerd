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
    class MyClass<T = void, U = void> {
      constructor(str: string): MyClass;
      prop: T;
      method(): U {}
      get accessor(): number { return 42; }
      static staticMethod(str?: string): void {}
      private privateMethod() {}
    }
  `;

  const expectedOutput = `
    declare var MyClass: {
      prototype: MyClass;
      new <T = void, U = void>(str: string): MyClass<T, U>;
      staticMethod(str?: string): void;
    };
    interface MyClass<T = void, U = void> {
      prop: T;
      method(): U;
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
