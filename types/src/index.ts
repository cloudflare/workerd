// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { StructureGroups } from '@workerd/jsg/rtti';
import ts from 'typescript';
import { generateDefinitions } from './generator';
import { PER_ISOLATE_MANIFEST } from './per-isolate';
import { printNodeList, printer } from './print';
import { SourcesMap, createMemoryProgram } from './program';
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
} from './transforms';
import { createClassToInterfaceTransformer } from './transforms/class-to-interface';
import { createAddOnMessageDeclarationTransformer } from './transforms/onmessage-declaration';

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
): string {
  const program = createMemoryProgram(sources);
  const checker = program.getTypeChecker();
  const sourceFile = program.getSourceFile(sourcePath);
  assert(sourceFile !== undefined);
  const result = ts.transform(sourceFile, transforms(program, checker));
  assert.strictEqual(result.transformed.length, 1);
  return printer.printFile(result.transformed[0]);
}

// The top-level names `source` declares.
function declaredNames(source: string): Set<string> {
  const file = ts.createSourceFile(
    'names.ts',
    source,
    ts.ScriptTarget.ESNext,
    false,
    ts.ScriptKind.TS
  );
  const names = new Set<string>();
  for (const statement of file.statements) {
    if (ts.isVariableStatement(statement)) {
      for (const declaration of statement.declarationList.declarations) {
        if (ts.isIdentifier(declaration.name)) {
          names.add(declaration.name.text);
        }
      }
    } else if (
      (ts.isClassDeclaration(statement) ||
        ts.isInterfaceDeclaration(statement) ||
        ts.isTypeAliasDeclaration(statement) ||
        ts.isEnumDeclaration(statement) ||
        ts.isFunctionDeclaration(statement) ||
        ts.isModuleDeclaration(statement)) &&
      statement.name !== undefined &&
      ts.isIdentifier(statement.name)
    ) {
      names.add(statement.name.text);
    }
  }
  return names;
}

// A name the bootstrap publishes has no other declaration to merge with: the
// C++ or Rust class it replaces declares itself `never`, and no define
// declares it.
function checkPerIsolateNames(
  perIsolate: string,
  generated: string,
  defines: string
): void {
  const generatedNames = declaredNames(generated);
  const defineNames = declaredNames(defines);
  const manifest = `src/per_isolate/${PER_ISOLATE_MANIFEST}`;
  for (const name of declaredNames(perIsolate)) {
    if (generatedNames.has(name)) {
      throw new Error(
        `${name} is published by ${manifest} and also declared by JSG RTTI; override the RTTI declaration to \`never\``
      );
    }
    if (defineNames.has(name)) {
      throw new Error(
        `${name} is published by ${manifest} and also declared in types/defines`
      );
    }
  }
}

// `perIsolateDefinitions` are the declarations the per-isolate bootstrap
// publishes (collectPerIsolateDeclarations()). They follow the generated
// declarations, with the standard comments but none of the transforms that
// adjust RTTI, and precede `extraDefinitions`, the defines.
export function printDefinitions(
  root: StructureGroups,
  commentData: CommentsData,
  extraDefinitions: string,
  perIsolateDefinitions = ''
): { ambient: string; importable: string } {
  // Generate TypeScript nodes from capnp request
  const { nodes, included } = generateDefinitions(root);

  // Assemble partial overrides and defines to valid TypeScript source files
  const [sources, replacements] = compileOverridesDefines(root, included);
  // Add source file containing generated nodes
  const sourcePath = '/$virtual/source.ts';
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
    createClassToInterfaceTransformer(['Request', 'Response', 'WebSocket']),
    // TODO: enable this once we've figured out how not to expose internal modules
    // createInternalNamespaceTransformer(root, structureMap),
    createCommentsTransformer(commentData),
    createAddOnMessageDeclarationTransformer(),
  ]);

  const perIsolatePath = '/$virtual/per-isolate.ts';
  const perIsolate = transform(
    new SourcesMap([[perIsolatePath, perIsolateDefinitions]]),
    perIsolatePath,
    () => [createCommentsTransformer(commentData)]
  );
  checkPerIsolateNames(perIsolate, source, extraDefinitions);

  // TODO: enable this once we've figured out how not to expose internal modules
  // source += collectTypeScriptModules(root) + extraDefinitions;
  source += perIsolate + extraDefinitions;

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
