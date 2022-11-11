import assert from "assert";
import { test } from "node:test";
import path from "path";
import ts from "typescript";
import { printer } from "../../src/print";
import { createMemoryProgram } from "../../src/program";
import { createIteratorTransformer } from "../../src/transforms";

test("createIteratorTransformer: replaces Iterator-like interfaces with built-in Iterators", () => {
  const source = `export class Thing {
    readonly thingsProperty: ThingIterator;
    readonly asyncThingsProperty: AsyncThingIterator;
    things(): ThingIterator;
    asyncThings(): AsyncThingIterator;
    [Symbol.iterator](): ThingIterator;
    [Symbol.asyncIterator](): AsyncThingIterator;
}
export interface ThingIterator extends Iterator {
    next(): ThingIteratorNext;
    [Symbol.iterator](): any;
}
export interface ThingIteratorNext {
    done: boolean;
    value?: string;
}
export interface AsyncThingIterator extends AsyncIterator {
    next(): Promise<AsyncThingIteratorNext>;
    return(value?: any): Promise<AsyncThingIteratorNext>;
    [Symbol.asyncIterator](): any;
}
export interface AsyncThingIteratorNext {
  done: boolean;
  value?: number;
}
`;
  const sourcePath = path.resolve(__dirname, "source.ts");
  const sources = new Map([[sourcePath, source]]);
  const program = createMemoryProgram(sources);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  const result = ts.transform(sourceFile, [createIteratorTransformer(checker)]);
  assert.strictEqual(result.transformed.length, 1);

  const output = printer.printFile(result.transformed[0]);
  assert.strictEqual(
    output,
    `export class Thing {
    readonly thingsProperty: IterableIterator<string>;
    readonly asyncThingsProperty: AsyncIterableIterator<number>;
    things(): IterableIterator<string>;
    asyncThings(): AsyncIterableIterator<number>;
    [Symbol.iterator](): IterableIterator<string>;
    [Symbol.asyncIterator](): AsyncIterableIterator<number>;
}
`
  );
});
