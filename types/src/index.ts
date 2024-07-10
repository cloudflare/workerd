// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import { StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import { Message } from "capnp-ts";
import ts from "typescript";
import { collectTypeScriptModules, generateDefinitions } from "./generator";
import { printNodeList, printer } from "./print";
import { SourcesMap, createMemoryProgram } from "./program";
import {
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

function printCppDefinitions(root: StructureGroups) {
  // Generate TypeScript nodes from capnp request
  const { nodes, structureMap } = generateDefinitions(root);

  // Assemble partial overrides and defines to valid TypeScript source files
  const [sources, replacements] = compileOverridesDefines(root);
  // Add source file containing generated nodes
  const sourcePath = "/$virtual/cpp-source.ts";
  sources.set(sourcePath, printNodeList(nodes));

  // Run post-processing transforms on program
  return transform(sources, sourcePath, (program, checker) => [
    // Run iterator transformer before overrides so iterator-like interfaces are
    // still removed if they're replaced in overrides
    createIteratorTransformer(checker),
    createOverrideDefineTransformer(program, replacements),
    // Run global scope transformer after overrides so members added in
    // overrides are extracted
    createGlobalScopeTransformer(checker),
    createInternalNamespaceTransformer(root, structureMap),
    createCommentsTransformer(),
  ]);
}

function printTsDefinitions(root: StructureGroups, extraDefinitions: string) {
  let source = collectTypeScriptModules(root);
  source += extraDefinitions;
  const sourcePath = "/$virtual/ts-source.ts";
  const sources = new SourcesMap([[sourcePath, source]]);

  return transform(sources, sourcePath, () => [
    createImportResolveTransformer(),
    createAmbientTransformer(),
  ]);
}

export function makeImportable(definitions: string) {
  const sourcePath = "/$virtual/source.ts";
  const sources = new SourcesMap([[sourcePath, definitions]]);
  return transform(sources, sourcePath, () => [createImportableTransformer()]);
}

export interface BuildTypesOptions {
  rttiCapnpBuffer: ArrayBuffer | Uint8Array;
  extraDefinitions: string;
}

export function buildTypes(opts: BuildTypesOptions) {
  const message = new Message(opts.rttiCapnpBuffer, /* packed */ false);
  const root = message.getRoot(StructureGroups);
  const cppDefinitions = printCppDefinitions(root);
  const tsDefinitions = printTsDefinitions(root, opts.extraDefinitions);
  return definitionsHeader + cppDefinitions + tsDefinitions;
}

export {
  ParameterNamesData,
  installParameterNames,
} from "./generator/parameter-names";
export { CommentsData, installComments } from "./transforms";
export { createMemoryProgram } from "./program";
