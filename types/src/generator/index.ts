// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import {
  FunctionType,
  Member,
  Member_Which,
  Method,
  Structure,
  StructureGroups,
  Type,
  Type_Which,
} from "@workerd/jsg/rtti.capnp.js";
import ts, { factory as f } from "typescript";
import { createStructureNode } from "./structure";
import { getTypeName } from "./type";

export { getTypeName } from "./type";

export type StructureMap = Map<string, Structure>;
// Builds a lookup table mapping type names to structures
function collectStructureMap(root: StructureGroups): StructureMap {
  const map = new Map<string, Structure>();
  root.getGroups().forEach((group) => {
    group.getStructures().forEach((structure) => {
      map.set(structure.getFullyQualifiedName(), structure);
    });
  });
  return map;
}

// Builds a set containing the names of structures that should be included
// in the definitions, because they are referenced by root types or any of their
// children. A struct/resource type is marked as a root type using a
// `JSG_(STRUCT_)TS_ROOT` macro.
//
// We need to do this as some types should only be included in the definitions
// when certain compatibility flags are enabled (e.g. `Navigator`,
// standards-compliant `URL`). However, these types are always included in
// the `*_TYPES` macros.
function collectIncluded(map: StructureMap, root?: string): Set<string> {
  const included = new Set<string>();

  function visitType(type: Type): void {
    switch (type.which()) {
      case Type_Which.PROMISE:
        return visitType(type.getPromise().getValue());
      case Type_Which.STRUCTURE:
        const name = type.getStructure().getFullyQualifiedName();
        const structure = map.get(name);
        assert(structure !== undefined, `Unknown structure type: ${name}`);
        return visitStructure(structure);
      case Type_Which.ARRAY:
        return visitType(type.getArray().getElement());
      case Type_Which.MAYBE:
        return visitType(type.getMaybe().getValue());
      case Type_Which.DICT:
        const dict = type.getDict();
        visitType(dict.getKey());
        return visitType(dict.getValue());
      case Type_Which.ONE_OF:
        return type.getOneOf().getVariants().forEach(visitType);
      case Type_Which.FUNCTION:
        return visitFunction(type.getFunction());
    }
  }

  function visitFunction(func: FunctionType | Method) {
    func.getArgs().forEach(visitType);
    visitType(func.getReturnType());
  }

  function visitMember(member: Member) {
    switch (member.which()) {
      case Member_Which.METHOD:
        return visitFunction(member.getMethod());
      case Member_Which.PROPERTY:
        return visitType(member.getProperty().getType());
      case Member_Which.NESTED:
        return visitStructure(member.getNested().getStructure());
      case Member_Which.CONSTRUCTOR:
        return member.getConstructor().getArgs().forEach(visitType);
    }
  }

  function visitStructure(structure: Structure): void {
    const name = structure.getFullyQualifiedName();
    if (included.has(name)) return;
    included.add(name);
    structure.getMembers().forEach(visitMember);
    if (structure.hasExtends()) {
      visitType(structure.getExtends());
    }
    if (structure.hasIterator()) {
      visitFunction(structure.getIterator());
    }
    if (structure.hasAsyncIterator()) {
      visitFunction(structure.getAsyncIterator());
    }
  }

  if (root === undefined) {
    // If no root was specified, visit all structures with
    // `JSG_(STRUCT_)TS_ROOT` macros
    for (const structure of map.values()) {
      if (structure.getTsRoot()) visitStructure(structure);
    }
  } else {
    // Otherwise, visit just that root
    const structure = map.get(root);
    assert(structure !== undefined, `Unknown root: ${root}`);
    visitStructure(structure);
  }

  return included;
}

// Builds a set containing the names of structures that must be declared as
// `class`es rather than `interface`s because they either:
// 1) Get inherited by another class (`class` `extends` requires another `class`)
// 2) Are constructible (`constructor(...)`s can only appear in `class`es)
// 3) Have `static` methods (`static`s can only appear in `class`es)
// 4) Are a nested type (users could call `instanceof` with the type)
function collectClasses(map: StructureMap): Set<string> {
  const classes = new Set<string>();
  for (const structure of map.values()) {
    // 1) Add all classes inherited by this class
    if (structure.hasExtends()) {
      const extendsType = structure.getExtends();
      if (extendsType.isStructure()) {
        classes.add(extendsType.getStructure().getFullyQualifiedName());
      }
    }

    structure.getMembers().forEach((member) => {
      // 2) Add this class if it's constructible
      if (member.isConstructor()) {
        classes.add(structure.getFullyQualifiedName());
      }
      // 3) Add this class if it contains static methods
      if (member.isMethod() && member.getMethod().getStatic()) {
        classes.add(structure.getFullyQualifiedName());
      }
      // 4) Add all nested types defined by this class
      if (member.isNested()) {
        classes.add(member.getNested().getStructure().getFullyQualifiedName());
      }
    });
  }
  return classes;
}

// Builds a map mapping structure names that are top-level nested types of
// module structures to the names of those modules. Essentially, a map of which
// modules export which types (e.g. "workerd::api::node::AsyncLocalStorage" =>
// "node-internal:async_hooks"). We use this to make sure we don't include
// duplicate definitions if an internal module references a type from another
// internal module. In this case, we'll include the definition in the one that
// exported it.
function collectModuleTypeExports(
  root: StructureGroups,
  map: StructureMap
): Map</* structureName */ string, /* moduleSpecifier */ string> {
  const typeExports = new Map<string, string>();
  root.getModules().forEach((module) => {
    if (!module.isStructureName()) return;

    // Get module root type
    const specifier = module.getSpecifier();
    const moduleRootName = module.getStructureName();
    const moduleRoot = map.get(moduleRootName);
    assert(moduleRoot !== undefined);

    // Add all nested types in module root
    moduleRoot.getMembers().forEach((member) => {
      if (!member.isNested()) return;
      const nested = member.getNested();
      typeExports.set(nested.getStructure().getFullyQualifiedName(), specifier);
    });
  });

  return typeExports;
}

export function generateDefinitions(root: StructureGroups): {
  nodes: ts.Statement[];
  structureMap: StructureMap;
} {
  const structureMap = collectStructureMap(root);
  const globalIncluded = collectIncluded(structureMap);
  const classes = collectClasses(structureMap);

  // Can't use `flatMap()` here as `getGroups()` returns a `capnp.List`
  const nodes = root.getGroups().map((group) => {
    const structureNodes: ts.Statement[] = [];
    group.getStructures().forEach((structure) => {
      const name = structure.getFullyQualifiedName();
      if (globalIncluded.has(name)) {
        const asClass = classes.has(name);
        structureNodes.push(createStructureNode(structure, { asClass }));
      }
    });
    return structureNodes;
  });
  const flatNodes = nodes.flat();

  const typeExports = collectModuleTypeExports(root, structureMap);
  root.getModules().forEach((module) => {
    if (!module.isStructureName()) return;

    // Get module root type
    const specifier = module.getSpecifier();
    const moduleRootName = module.getStructureName();
    const moduleRoot = structureMap.get(moduleRootName);
    assert(moduleRoot !== undefined);

    // Build a set of nested types exported by this module. These will always
    // be included in the module, even if they're referenced globally.
    const nestedTypeNames = new Set<string>();
    moduleRoot.getMembers().forEach((member) => {
      if (member.isNested()) {
        const nested = member.getNested();
        nestedTypeNames.add(nested.getStructure().getFullyQualifiedName());
      }
    });

    // Add all types required by this module, but not the top level or another
    // internal module.
    const moduleIncluded = collectIncluded(structureMap, moduleRootName);
    const statements: ts.Statement[] = [];

    let nextImportId = 1;
    for (const name of moduleIncluded) {
      // If this structure was already included globally, ignore it,
      // unless it's explicitly declared a nested type of this module
      if (globalIncluded.has(name) && !nestedTypeNames.has(name)) continue;

      // If this structure was exported by another module, import it. Note we
      // don't need to check whether we've already imported the type as
      // `moduleIncluded` is a `Set`.
      const maybeOwningModule = typeExports.get(name);
      if (maybeOwningModule !== undefined && maybeOwningModule !== specifier) {
        // Internal modules only have default exports, so we generate something
        // that looks like this:
        // ```
        // import _internal1 from "node-internal:async_hooks";
        // import AsyncLocalStorage = _internal1.AsyncLocalStorage; // (type & value alias)
        // ```
        const identifier = f.createIdentifier(`_internal${nextImportId++}`);
        const importClause = f.createImportClause(
          false,
          /* name */ identifier,
          /* namedBindings */ undefined
        );
        const importDeclaration = f.createImportDeclaration(
          /* modifiers */ undefined,
          importClause,
          f.createStringLiteral(maybeOwningModule)
        );
        const typeName = getTypeName(name);
        const importEqualsDeclaration = f.createImportEqualsDeclaration(
          /* modifiers */ undefined,
          /* isTypeOnly */ false,
          typeName,
          f.createQualifiedName(identifier, typeName)
        );
        statements.unshift(importDeclaration, importEqualsDeclaration);

        continue;
      }

      // Otherwise, just include the structure in the module
      const structure = structureMap.get(name);
      assert(structure !== undefined);
      const asClass = classes.has(name);
      const statement = createStructureNode(structure, {
        asClass,
        ambientContext: true,
        // nameOverride: nestedNameOverrides.get(name), // TODO: remove
      });
      statements.push(statement);
    }

    const moduleBody = f.createModuleBlock(statements);
    const moduleDeclaration = f.createModuleDeclaration(
      [f.createToken(ts.SyntaxKind.DeclareKeyword)],
      f.createStringLiteral(specifier),
      moduleBody
    );
    flatNodes.push(moduleDeclaration);
  });

  return { nodes: flatNodes, structureMap };
}

export function collectTypeScriptModules(root: StructureGroups): string {
  let result = "";

  root.getModules().forEach((module) => {
    if (!module.isTsDeclarations()) return;
    const declarations = module
      .getTsDeclarations()
      // Looks for any lines starting with `///`, which indicates a TypeScript
      // Triple-Slash Directive (https://www.typescriptlang.org/docs/handbook/triple-slash-directives.html)
      .replaceAll(/^\/\/\/.+$/gm, (match) => {
        assert.strictEqual(
          match,
          '/// <reference types="@workerd/types-internal" />',
          `Unexpected triple-slash directive, got ${match}`
        );
        return "";
      });

    result += `declare module "${module.getSpecifier()}" {\n${declarations}\n}\n`;
  });

  return result;
}
