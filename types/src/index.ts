#!/usr/bin/env node
import assert from "assert";
import { mkdir, readFile, writeFile } from "fs/promises";
import path from "path";
import { arrayBuffer } from "stream/consumers";
import util from "util";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import prettier from "prettier";
import ts from "typescript";
import { generateDefinitions } from "./generator";
import { printNodeList, printer } from "./print";
import { createMemoryProgram } from "./program";
import {
  compileOverridesDefines,
  createGlobalScopeTransformer,
  createIteratorTransformer,
  createOverrideDefineTransformer,
} from "./transforms";

const definitionsHeader = `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
`;

function printDefinitions(root: StructureGroups): string {
  // Generate TypeScript nodes from capnp request
  const nodes = generateDefinitions(root);

  // Assemble partial overrides and defines to valid TypeScript source files
  const [sources, replacements] = compileOverridesDefines(root);
  // Add source file containing generated nodes
  const sourcePath = path.resolve(__dirname, "source.ts");
  let source = printNodeList(nodes);
  sources.set(sourcePath, source);

  // Build TypeScript program from source files and overrides. Importantly,
  // these are in the same program, so we can use nodes from one in the other.
  let program = createMemoryProgram(sources);
  let checker = program.getTypeChecker();
  let sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  // Run post-processing transforms on program
  let result = ts.transform(sourceFile, [
    // Run iterator transformer before overrides so iterator-like interfaces are
    // still removed if they're replaced in overrides
    createIteratorTransformer(checker),
    createOverrideDefineTransformer(program, replacements),
  ]);
  assert.strictEqual(result.transformed.length, 1);

  // We need the type checker to respect our updated definitions after applying
  // overrides (e.g. to find the correct nodes when traversing heritage), so
  // rebuild the program to re-run type checking.
  // TODO: is there a way to re-run the type checker on an existing program?
  source = printer.printFile(result.transformed[0]);
  program = createMemoryProgram(new Map([[sourcePath, source]]));
  checker = program.getTypeChecker();
  sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);

  result = ts.transform(sourceFile, [
    // Run global scope transformer after overrides so members added in
    // overrides are extracted
    createGlobalScopeTransformer(checker),
    // TODO(polish): maybe flatten union types?
  ]);
  assert.strictEqual(result.transformed.length, 1);

  // TODO(polish): maybe log diagnostics with `ts.getPreEmitDiagnostics(program, sourceFile)`?
  //  (see https://github.com/microsoft/TypeScript/wiki/Using-the-Compiler-API#a-minimal-compiler)

  // Print program to string
  return definitionsHeader + printer.printFile(result.transformed[0]);
}

// Generates TypeScript types from a binary Cap’n Proto file containing encoded
// JSG RTTI. See src/workerd/tools/api-encoder.c++ for a script that generates
// input expected by this tool.
//
// To generate types using default options, run `bazel build //types:types`.
//
// Usage: types [options] [input]
//
// Options:
//  -o, --output <file>
//    File path to write TypeScript to, defaults to stdout if omitted
//  -f, --format
//    Formats generated types with Prettier
//
// Input:
//    Binary Cap’n Proto file path, defaults to reading from stdin if omitted
export async function main(args?: string[]) {
  const { values: options, positionals } = util.parseArgs({
    options: {
      output: { type: "string", short: "o" },
      format: { type: "boolean", short: "f" },
    },
    strict: true,
    allowPositionals: true,
    args,
  });
  const maybeInputPath = positionals[0];

  const buffer =
    maybeInputPath === undefined
      ? await arrayBuffer(process.stdin)
      : await readFile(maybeInputPath);
  const message = new Message(buffer, /* packed */ false);
  const root = message.getRoot(StructureGroups);

  let definitions = printDefinitions(root);
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
