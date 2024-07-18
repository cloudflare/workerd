import assert from "node:assert";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import ts from "typescript";
import { generateDefinitions } from "./generator";
import { printNodeList, printer } from "./print";
import { SourcesMap, createMemoryProgram } from "./program";
import {
  CommentsData,
  compileOverridesDefines,
  createAmbientTransformer,
  createCommentsTransformer,
  createGlobalScopeTransformer,
  createImportResolveTransformer,
  createImportableTransformer,
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

export function printDefinitions(
  root: StructureGroups,
  commentData: CommentsData,
  extraDefinitions: string
): { ambient: string; importable: string } {
  // Generate TypeScript nodes from capnp request
  const { nodes } = generateDefinitions(root);

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
    // TODO: enable this once we've figured out how not to expose internal modules
    // createInternalNamespaceTransformer(root, structureMap),
    createCommentsTransformer(commentData),
  ]);

  // TODO: enable this once we've figured out how not to expose internal modules
  // source += collectTypeScriptModules(root) + extraDefinitions;
  source += extraDefinitions;

  // We need the type checker to respect our updated definitions after applying
  // overrides (e.g. to find the correct nodes when traversing heritage), so
  // rebuild the program to re-run type checking. We also want to include our
  // additional definitions.
  source = transform(new SourcesMap([[sourcePath, source]]), sourcePath, () => [
    createImportResolveTransformer(),
    createAmbientTransformer(),
  ]);

  const importable = transform(
    new SourcesMap([[sourcePath, source]]),
    sourcePath,
    () => [createImportableTransformer()]
  );

  // Print program to string
  return {
    ambient: definitionsHeader + source,
    importable: definitionsHeader + importable,
  };
}
