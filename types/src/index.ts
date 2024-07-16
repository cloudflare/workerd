#!/usr/bin/env node
import assert from "node:assert";
import { readFileSync, readdirSync } from "node:fs";
import { mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import path from "node:path";
import util from "node:util";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import prettier from "prettier";
import ts from "typescript";
import cloudflareComments from "./cloudflare";
import { collectTypeScriptModules, generateDefinitions } from "./generator";
import { parseApiAstDump } from "./generator/parameter-names";
import { printNodeList, printer } from "./print";
import { SourcesMap, createMemoryProgram } from "./program";
import { collateStandardComments } from "./standards";
import {
  CommentsData,
  compileOverridesDefines,
  createAmbientTransformer,
  createCommentsTransformer,
  createGlobalScopeTransformer,
  createImportResolveTransformer,
  createImportableTransformer,
  createInternalNamespaceTransformer,
  createIteratorTransformer,
  createOverrideDefineTransformer,
} from "./transforms";

const definitionsHeader = `/*! *****************************************************************************
Copyright (c) Cloudflare. All rights reserved.
Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at http://www.apache.org/licenses/LICENSE-2.0
THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
MERCHANTABLITY OR NON-INFRINGEMENT.
See the Apache Version 2.0 License for specific language governing permissions
and limitations under the License.
***************************************************************************** */
/* eslint-disable */
// noinspection JSUnusedGlobalSymbols
`;

async function* walkDir(root: string): AsyncGenerator<string> {
  const entries = await readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    const entryPath = path.join(root, entry.name);
    if (entry.isDirectory()) yield* walkDir(entryPath);
    else yield entryPath;
  }
}

async function collateExtraDefinitions(definitionsDir?: string) {
  if (definitionsDir === undefined) return "";
  const files: Promise<string>[] = [];
  for await (const filePath of walkDir(path.resolve(definitionsDir))) {
    files.push(readFile(filePath, "utf8"));
  }
  return (await Promise.all(files)).join("\n");
}

/**
 * Copy all TS lib files into the memory filesystem. We only use lib.esnext
 * but since TS lib files all reference each other to various extents it's
 * easier to add them all and let TS figure out which ones it actually needs to load.
 * This function uses the current local installation of TS as a source for lib files
 */
function loadLibFiles(): SourcesMap {
  const libLocation = path.dirname(require.resolve("typescript"));
  const libFiles = readdirSync(libLocation).filter(
    (file) => file.startsWith("lib.") && file.endsWith(".d.ts")
  );
  const lib: SourcesMap = new Map();
  for (const file of libFiles) {
    lib.set(
      `/node_modules/typescript/lib/${file}`,
      readFileSync(path.join(libLocation, file), "utf-8")
    );
  }
  return lib;
}

function checkDiagnostics(sources: SourcesMap) {
  const program = createMemoryProgram(
    sources,
    undefined,
    {
      noEmit: true,
      lib: ["lib.esnext.d.ts"],
      types: [],
    },
    loadLibFiles()
  );

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
      console.log(
        `${diagnostic.file.fileName}:${line + 1}:${character + 1} : ${message}`
      );
    } else {
      console.log(
        ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n")
      );
    }
  });

  assert(allDiagnostics.length === 0, "TypeScript failed to compile!");
}

function transform(
  sources: SourcesMap,
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
  commentData: CommentsData,
  extraDefinitions: string
): { ambient: string; importable: string } {
  // Generate TypeScript nodes from capnp request
  const { nodes, structureMap } = generateDefinitions(root);

  // Assemble partial overrides and defines to valid TypeScript source files
  const [sources, replacements] = compileOverridesDefines(root);
  // Add source file containing generated nodes
  const sourcePath = "/$virtual/source.ts";
  let source = printNodeList(nodes);
  sources.set(sourcePath, printNodeList(nodes));

  // Run post-processing transforms on program
  source = transform(sources, sourcePath, (program, checker) => [
    // Run iterator transformer before overrides so iterator-like interfaces are
    // still removed if they're replaced in overrides
    createIteratorTransformer(checker),
    createOverrideDefineTransformer(program, replacements),
    // Run global scope transformer after overrides so members added in
    // overrides are extracted
    createGlobalScopeTransformer(checker),
    createInternalNamespaceTransformer(root, structureMap),
    createCommentsTransformer(commentData),
  ]);

  source += collectTypeScriptModules(root) + extraDefinitions;

  // We need the type checker to respect our updated definitions after applying
  // overrides (e.g. to find the correct nodes when traversing heritage), so
  // rebuild the program to re-run type checking. We also want to include our
  // additional definitions.
  source = transform(new SourcesMap([[sourcePath, source]]), sourcePath, () => [
    createImportResolveTransformer(),
    createAmbientTransformer(),
  ]);

  checkDiagnostics(new SourcesMap([[sourcePath, source]]));

  const importable = transform(
    new SourcesMap([[sourcePath, source]]),
    sourcePath,
    () => [createImportableTransformer()]
  );

  checkDiagnostics(new SourcesMap([[sourcePath, importable]]));

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
//  -d, --defines <dir>
//    Directory containing extra TypeScript definitions, not associated with C++
//    files, to concatenate to the output
//  -o, --output <dir>
//    Directory to write types to, in folders based on compat date
//  -f, --format
//    Formats generated types with Prettier
//
// Input:
//    Directory containing binary Cap’n Proto file paths, in the format <label>.api.capnp.bin
//      <label> should relate to the compatibility date
export async function main(args?: string[]) {
  const {
    values: {
      "input-dir": inputDir,
      "output-dir": outputDir,
      "defines-dir": definesDir,
      ...options
    },
  } = util.parseArgs({
    options: {
      "defines-dir": { type: "string", short: "d" },
      "input-dir": { type: "string", short: "i" },
      "output-dir": { type: "string", short: "o" },
      format: { type: "boolean", short: "f" },
    },
    strict: true,
    allowPositionals: false,
    args,
  });

  if (!inputDir) {
    throw new Error(
      "Expected an argument --input-dir pointing to a directory containing api.capnp.bin files and parameter names"
    );
  }

  const inputFiles = await readdir(inputDir).then((filenames) =>
    filenames.map((filename) => path.join(inputDir, filename))
  );

  const extra = await collateExtraDefinitions(definesDir);

  const paramNamesJson = inputFiles.find(
    (filepath) => path.basename(filepath) === "param-names.json"
  );
  const capnpFiles = inputFiles.filter((filename) =>
    path.basename(filename).endsWith("api.capnp.bin")
  );

  if (paramNamesJson === undefined) {
    console.warn(
      `Couldn't find param-names.json in ${inputDir} containing parameter names, params will be nameless.`
    );
  } else {
    parseApiAstDump(paramNamesJson);
  }

  if (capnpFiles.length === 0) {
    throw new Error(
      `Expected to find at least one file ending with api.capnp.bin in ${inputDir}`
    );
  }

  const comments = collateStandardComments(
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.d.ts"
    ),
    path.join(
      path.dirname(require.resolve("typescript")),
      "lib.webworker.iterable.d.ts"
    )
  );

  // We want to deep merge here so that our comment overrides can be very targeted
  for (const [name, members] of Object.entries(cloudflareComments)) {
    comments[name] ??= {};
    for (const [member, comment] of Object.entries(members)) {
      const apiEntry = comments[name];
      assert(apiEntry !== undefined);
      apiEntry[member] = comment;
    }
  }

  for (const file of capnpFiles) {
    const buffer = await readFile(file);
    const message = new Message(buffer, /* packed */ false);
    const root = message.getRoot(StructureGroups);
    let { ambient, importable } = printDefinitions(root, comments, extra);
    if (options.format) {
      // Strip `// prettier-ignore` comments. TypeScript will reformat all
      // defines anyway, and Prettier's formatting is nicer.
      const prettierIgnoreRegexp = /^\s*\/\/\s*prettier-ignore\s*\n/gm;
      ambient = ambient.replaceAll(prettierIgnoreRegexp, "");
      importable = importable.replaceAll(prettierIgnoreRegexp, "");

      ambient = await prettier.format(ambient, { parser: "typescript" });
      importable = await prettier.format(importable, { parser: "typescript" });
    }
    if (outputDir !== undefined) {
      const output = path.resolve(outputDir);

      const [date] = path.basename(file).split(".api.capnp.bin");
      await mkdir(path.join(output, date), { recursive: true });
      await writeFile(path.join(output, date, "index.d.ts"), ambient);
      const importableFile = path.join(output, date, "index.ts");
      await writeFile(importableFile, importable);
    }
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
