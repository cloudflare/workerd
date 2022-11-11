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
import { ParsedTypeDefinition, collateStandards } from "./standards";
import {
  compileOverridesDefines,
  createCommentsTransformer,
  createGlobalScopeTransformer,
  createIteratorTransformer,
  createOverrideDefineTransformer,
} from "./transforms";
import { createAmbientTransformer } from "./transforms/ambient";
import { createImportableTransformer } from "./transforms/importable";
const definitionsHeader = `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
`;

function checkDiagnostics(sources: Map<string, string>) {
  const host = ts.createCompilerHost(
    { noEmit: true },
    /* setParentNodes */ true
  );

  host.getDefaultLibLocation = () =>
    path.dirname(require.resolve("typescript"));
  const program = createMemoryProgram(sources, host, {
    lib: ["lib.esnext.d.ts"],
  });
  const emitResult = program.emit();

  const allDiagnostics = ts
    .getPreEmitDiagnostics(program)
    .concat(emitResult.diagnostics);

  allDiagnostics.forEach((diagnostic) => {
    if (diagnostic.file) {
      const { line, character } = ts.getLineAndCharacterOfPosition(
        diagnostic.file,
        diagnostic.start!
      );
      const message = ts.flattenDiagnosticMessageText(
        diagnostic.messageText,
        "\n"
      );
      console.log(`(${line + 1},${character + 1}): ${message}`);
    } else {
      console.log(
        ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n")
      );
    }
  });
}
function transform(
  sources: Map<string, string>,
  sourcePath: string,
  transforms: (
    program: ts.Program,
    checker: ts.TypeChecker
  ) => ts.TransformerFactory<ts.SourceFile>[]
) {
  const program = createMemoryProgram(sources);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);
  const result = ts.transform(sourceFile, transforms(program, checker));
  assert.strictEqual(result.transformed.length, 1);
  return printer.printFile(result.transformed[0]);
}
function printDefinitions(
  root: StructureGroups,
  standards: ParsedTypeDefinition
): { ambient: string; importable: string } {
  // Generate TypeScript nodes from capnp request
  const nodes = generateDefinitions(root);

  // Assemble partial overrides and defines to valid TypeScript source files
  const [sources, replacements] = compileOverridesDefines(root);
  // Add source file containing generated nodes
  const sourcePath = path.resolve(__dirname, "source.ts");
  let source = printNodeList(nodes);
  sources.set(sourcePath, source);

  // Run post-processing transforms on program
  source = transform(sources, sourcePath, (program, checker) => [
    // Run iterator transformer before overrides so iterator-like interfaces are
    // still removed if they're replaced in overrides
    createIteratorTransformer(checker),
    createOverrideDefineTransformer(program, replacements),
  ]);

  // We need the type checker to respect our updated definitions after applying
  // overrides (e.g. to find the correct nodes when traversing heritage), so
  // rebuild the program to re-run type checking.
  // TODO: is there a way to re-run the type checker on an existing program?
  source = transform(
    new Map([[sourcePath, source]]),
    sourcePath,
    (program, checker) => [
      // Run global scope transformer after overrides so members added in
      // overrides are extracted
      createGlobalScopeTransformer(checker),
      createCommentsTransformer(standards),
      createAmbientTransformer(),
      // TODO(polish): maybe flatten union types?
    ]
  );

  checkDiagnostics(new Map([[sourcePath, source]]));

  const importable = transform(
    new Map([[sourcePath, source]]),
    sourcePath,
    () => [createImportableTransformer()]
  );

  checkDiagnostics(new Map([[sourcePath, importable]]));

  // Print program to string
  return {
    ambient: definitionsHeader + source,
    importable: definitionsHeader + importable,
  };
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

  const standards = await collateStandards(
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.d.ts"
    ),
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.iterable.d.ts"
    )
  );

  let { ambient, importable } = printDefinitions(root, standards);

  if (options.format) {
    ambient = prettier.format(ambient, { parser: "typescript" });
    importable = prettier.format(importable, { parser: "typescript" });
  }
  if (options.output !== undefined) {
    console.log(options.output);
    const output = path.resolve(options.output);
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, ambient);

    const importableFile = path.join(path.dirname(output), "api.ts");
    await writeFile(importableFile, importable);
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
