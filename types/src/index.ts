#!/usr/bin/env node
import assert from "assert";
import { mkdir, readFile, writeFile } from "fs/promises";
import path from "path";
import { arrayBuffer } from "stream/consumers";
import util from "util";
import { DefinitionGeneratorRequest } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import prettier from "prettier";
import ts from "typescript";
import { generateDefinitions } from "./generator";
import { printNodeList, printer } from "./print";
import { createMemoryProgram } from "./program";
import {
  createGlobalScopeTransformer,
  createIteratorTransformer,
} from "./transforms";

const definitionsHeader = `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
`;

function printDefinitions(req: DefinitionGeneratorRequest): string {
  // Generate TypeScript nodes from capnp request
  const nodes = generateDefinitions(req);

  // Build TypeScript program from nodes
  const source = printNodeList(nodes);
  // TODO(soon): when we switch to outputting a separate file per group, we'll
  //  need to modify this function to accept multiple source files
  //  (will probably need `program.getSourceFiles()`)
  const [program, sourcePath] = createMemoryProgram(source);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  // Run post-processing transforms on program
  const result = ts.transform(sourceFile, [
    // TODO(soon): when overrides are implemented, apply renames here
    createIteratorTransformer(checker),
    createGlobalScopeTransformer(checker),
    // TODO(polish): maybe flatten union types?
  ]);
  // TODO(polish): maybe log diagnostics with `ts.getPreEmitDiagnostics(program, sourceFile)`?
  //  (see https://github.com/microsoft/TypeScript/wiki/Using-the-Compiler-API#a-minimal-compiler)
  assert.strictEqual(result.transformed.length, 1);

  // Print program to string
  return definitionsHeader + printer.printFile(result.transformed[0]);
}

async function main() {
  const { values: options, positionals } = util.parseArgs({
    options: {
      output: { type: "string", short: "o" },
      format: { type: "boolean", short: "f" },
    },
    strict: true,
    allowPositionals: true,
  });
  const maybeInputPath = positionals[0];

  const buffer =
    maybeInputPath === undefined
      ? await arrayBuffer(process.stdin)
      : await readFile(maybeInputPath);
  const message = new Message(buffer, /* packed */ false);
  const req = message.getRoot(DefinitionGeneratorRequest);

  let definitions = printDefinitions(req);
  if (options.format) {
    definitions = prettier.format(definitions, { parser: "typescript" });
  }
  if (options.output !== undefined) {
    const output = path.resolve(options.output);
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, definitions);
  } else {
    // Write to stdout without extra newline
    process.stdout.write(definitions);
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
