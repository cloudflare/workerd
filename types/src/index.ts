#!/usr/bin/env node
import assert from "assert";
import { appendFile, mkdir, readFile, writeFile } from "fs/promises";
import path from "path";
import { arrayBuffer } from "stream/consumers";
import util from "util";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import prettier from "prettier";
import ts from "typescript";
import { generateDefinitions } from "./generator";
import postProcess from "./postprocess/parse";
import { printNodeList, printer } from "./print";
import { createMemoryProgram } from "./program";
import {
  compileOverridesDefines,
  createCommentsTransformer,
  createGlobalScopeTransformer,
  createIteratorTransformer,
  createOverrideDefineTransformer,
} from "./transforms";
import { createAmbientTransformer } from "./transforms/ambient";
import { createExportableTransformer } from "./transforms/exportable";
import { writeFileSync } from "fs";
const definitionsHeader = `/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
`;
export interface TypeDefinition {
  program: ts.Program;
  source: ts.SourceFile;
  checker: ts.TypeChecker;
}

export interface ParsedTypeDefinition extends TypeDefinition {
  parsed: {
    functions: Map<string, ts.FunctionDeclaration>;
    interfaces: Map<string, ts.InterfaceDeclaration>;
    vars: Map<string, ts.VariableDeclaration>;
    types: Map<string, ts.TypeAliasDeclaration>;
    classes: Map<string, ts.ClassDeclaration>;
  };
}

// Collate standards (to support lib.(dom|webworker).iterable.d.ts being defined separately)
async function collateStandards(
  ...standardTypes: string[]
): Promise<ParsedTypeDefinition> {
  const STANDARDS_PATH = "./tmp.standards.d.ts";
  await writeFile(STANDARDS_PATH, "");
  await Promise.all(
    standardTypes.map(
      async (s) =>
        await appendFile(
          STANDARDS_PATH,
          // Remove the Microsoft copyright notices from the file, to prevent them being copied in as TS comments
          (await readFile(s, "utf-8")).split(`/////////////////////////////`)[2]
        )
    )
  );
  const program = ts.createProgram(
    [STANDARDS_PATH],
    {},
    ts.createCompilerHost({}, true)
  );
  const source = program.getSourceFile(STANDARDS_PATH)!;
  const checker = program.getTypeChecker();
  const parsed = {
    functions: new Map<string, ts.FunctionDeclaration>(),
    interfaces: new Map<string, ts.InterfaceDeclaration>(),
    vars: new Map<string, ts.VariableDeclaration>(),
    types: new Map<string, ts.TypeAliasDeclaration>(),
    classes: new Map<string, ts.ClassDeclaration>(),
  };
  ts.forEachChild(source, (node) => {
    let name = "";
    if (node && ts.isFunctionDeclaration(node)) {
      name = node.name?.text ?? "";
      parsed.functions.set(name, node);
    } else if (ts.isVariableStatement(node)) {
      name = node.declarationList.declarations[0].name.getText(source);
      assert(node.declarationList.declarations.length === 1);
      parsed.vars.set(name, node.declarationList.declarations[0]);
    } else if (ts.isInterfaceDeclaration(node)) {
      name = node.name.text;
      parsed.interfaces.set(name, node);
    } else if (ts.isTypeAliasDeclaration(node)) {
      name = node.name.text;
      parsed.types.set(name, node);
    } else if (ts.isClassDeclaration(node)) {
      name = node.name?.text ?? "";
      parsed.classes.set(name, node);
    }
  });
  return {
    program,
    source,
    checker,
    parsed,
  };
}
function checkDiagnostics(sources: Map<string, string>) {
  const host = ts.createCompilerHost({}, /* setParentNodes */ true);

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
): { ambient: string; exportable: string } {
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

  const exportable = transform(
    new Map([[sourcePath, source]]),
    sourcePath,
    () => [createExportableTransformer()]
  );

  checkDiagnostics(new Map([[sourcePath, exportable]]));

  // Print program to string
  return {
    ambient: definitionsHeader + source,
    exportable: definitionsHeader + exportable,
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

  let { ambient, exportable } = printDefinitions(root, standards);
  // const output = path.resolve("tmp.api.d.ts");
  // await mkdir(path.dirname(output), { recursive: true });
  // await writeFile(output, definitions);

  // let { ambient, exportable } = await postProcess(
  //   output,

  //   path.join(
  //     path.dirname(require.resolve("typescript")),
  //     "lib.webworker.d.ts"
  //   ),
  //   path.join(
  //     path.dirname(require.resolve("typescript")),
  //     "lib.webworker.iterable.d.ts"
  //   )
  // );
  if (options.format) {
    ambient = prettier.format(ambient, { parser: "typescript" });
    exportable = prettier.format(exportable, { parser: "typescript" });
  }
  if (options.output !== undefined) {
    console.log(options.output);
    const output = path.resolve(options.output);
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, ambient);

    const exportableFile = path.join(path.dirname(output), "api.ts");
    await writeFile(exportableFile, exportable);
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
