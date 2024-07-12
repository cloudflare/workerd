// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import { Structure, StructureGroups } from "@workerd/jsg/rtti.capnp.js";
import ts from "typescript";
import { StructureMap, getTypeName } from "../generator";
import { maybeExtractGlobalNode } from "./globals";
import { ensureStatementModifiers } from "./helpers";
import { createRenameVisitor } from "./overrides";

// Moves all members (excluding imports) of internal `declare module` blocks
// into namespaces that are then `export default`ed:
//
// ```ts
// declare module "node-internal:diagnostics_channel" {
//   import _internal1 from "node-internal:async_hooks";
//   import AsyncLocalStorage = _internal1.AsyncLocalStorage;
//   interface DiagnosticsChannelModule {
//     readonly property: boolean;
//     channel<T>(key: PropertyKey): Channel<T>;
//     bindStore(store: AsyncLocalStorage, ...): void;
//     Channel: typeof Channel;
//   }
//   abstract class Channel<T> {
//     ...
//   }
// }
// ```
//
// --- transforms to --->
//
// ```ts
// declare module "node-internal:diagnostics_channel" {
//   import _internal1 from "node-internal:async_hooks";
//   import AsyncLocalStorage = _internal1.AsyncLocalStorage;
//   namespace _default {
//     const property: boolean;
//     function channel<T>(key: PropertyKey): Channel<T>;
//     function bindStore(store: AsyncLocalStorage, ...): void;
//     abstract class Channel<T> {
//       ...
//     }
//   }
//   export default _default;
// }
// ```
//
// Note we can't just generate the `namespace _default { ... }` when initially
// building the AST from RTTI, as the overrides/defines transformer only
// operates on `interface`/ `class`es, not bare `const`/`function`s.
//
// An alternative to exporting a `namespace` would be to export an instance of
// the root module type:
//
// ```
// declare module "node-internal:diagnostics_channel" {
//   ...
//   const _default: DiagnosticsChannelModule;
//   export default _default;
// }
// ```
//
// Whilst this correctly exports *values*, it doesn't export *types*.
// Importantly, it erases type parameters from generic types (i.e. `Channel<T>`
// becomes `Channel`, with `T`s becoming `unknown`).
export function createInternalNamespaceTransformer(
  root: StructureGroups,
  structureMap: StructureMap
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const moduleStructures = collectInternalModuleStructures(
        root,
        structureMap
      );
      const visitor = createInternalNamespaceVisitor(moduleStructures, ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function collectInternalModuleStructures(
  root: StructureGroups,
  structureMap: StructureMap
) {
  const moduleRoots = new Map</* specifier */ string, Structure>(); // TODO: add members here as well?
  root.getModules().forEach((module) => {
    if (!module.isStructureName()) return;
    const structure = structureMap.get(module.getStructureName());
    assert(structure !== undefined);
    const specifier = module.getSpecifier();
    moduleRoots.set(specifier, structure);
  });
  return moduleRoots;
}

function createInternalNamespaceVisitor(
  moduleRoots: ReturnType<typeof collectInternalModuleStructures>,
  ctx: ts.TransformationContext
) {
  const visitor: ts.Visitor = (node) => {
    if (
      ts.isModuleDeclaration(node) &&
      (node.flags & ts.NodeFlags.Namespace) === 0 &&
      ts.isStringLiteral(node.name) &&
      node.body !== undefined &&
      ts.isModuleBlock(node.body)
    ) {
      // This transformer should only be called on types generated from C++.
      // Therefore, all `declare modules` represent internal modules.
      const moduleRoot = moduleRoots.get(node.name.text);
      assert(
        moduleRoot !== undefined,
        `Expected "${node.name.text}" to be an internal module`
      );
      const moduleRootName = getTypeName(moduleRoot);

      // Ensure all nested types have the correct name. We do this after
      // overrides as some module members would otherwise have the same name as
      // global types (e.g. `cloudflare:workers` `DurableObjectBase` and
      // `DurableObject`)
      const renames = new Map</* from */ string, /* to */ string>();
      moduleRoot.getMembers().forEach((member) => {
        if (member.isNested()) {
          const nested = member.getNested();
          const generatedName = getTypeName(nested.getStructure());
          const actualName = nested.getName();
          if (generatedName !== actualName) {
            renames.set(generatedName, actualName);
          }
        }
      });

      // Filter array of statements to keep at the top-level of the module, and
      // build array of statements to include in default namespace export
      const namespaceStatements: ts.Statement[] = [];
      const moduleStatements = node.body.statements.filter((statement) => {
        if (
          ts.isImportDeclaration(statement) ||
          ts.isImportEqualsDeclaration(statement)
        ) {
          // Keep import statements at top-level of module
          return true;
        } else if (
          ts.isInterfaceDeclaration(statement) &&
          statement.name.text === moduleRootName
        ) {
          // Extract out properties and functions from the internal module root
          // type. Note internal module root types never have constructors, so
          // should always be interfaces. Internal module root types should
          // never have type parameters too, so we don't need to worry about
          // inlining type arguments, unlike the global scope visitor.
          for (const member of statement.members) {
            const maybeNode = maybeExtractGlobalNode(ctx, member);
            if (maybeNode !== undefined) namespaceStatements.push(maybeNode);
          }
          // Remove the root type from the module top-level
          return false;
        } else {
          // Assume all other class/interface definitions are nested types that
          // should be included in the default namespace export...
          namespaceStatements.push(
            ensureStatementModifiers(ctx, statement, { declare: false })
          );
          // ...and removed from the module top-level
          return false;
        }
      });

      // Add default namespace export to top-level statements
      const defaultIdentifier = ctx.factory.createIdentifier("_default");
      const namespaceBody = ctx.factory.createModuleBlock(namespaceStatements);
      const namespaceDeclaration = ctx.factory.createModuleDeclaration(
        /* modifiers */ undefined,
        defaultIdentifier,
        namespaceBody,
        ts.NodeFlags.Namespace
      );
      const exportStatement = ctx.factory.createExportAssignment(
        /* modifiers */ undefined,
        /* isExportEquals */ undefined,
        defaultIdentifier
      );
      moduleStatements.push(namespaceDeclaration, exportStatement);

      // Return updated module declaration with new top-level statements
      let body = ctx.factory.updateModuleBlock(node.body, moduleStatements);
      if (renames.size > 0) {
        const renameVisitor = createRenameVisitor(
          ctx,
          renames,
          /* renameClassesInterfaces */ true
        );
        body = ts.visitEachChild(body, renameVisitor, ctx);
      }
      return ctx.factory.updateModuleDeclaration(
        node,
        node.modifiers,
        node.name,
        body
      );
    }

    return node;
  };
  return visitor;
}
