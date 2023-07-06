// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import * as ts from "typescript";
import { printNode } from "../print";
// Checks whether the modifiers array contains a modifier of the specified kind
export function hasModifier(
  modifiers: ts.ModifiersArray,
  kind: ts.Modifier["kind"]
) {
  let hasModifier = false;
  modifiers?.forEach((modifier) => {
    hasModifier ||= modifier.kind === kind;
  });
  return hasModifier;
}

// Ensure a modifiers array has the specified modifier, inserting it at the
// start if it doesn't.
export function ensureModifier(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined,
  ensure: ts.SyntaxKind.ExportKeyword | ts.SyntaxKind.DeclareKeyword
): ts.ModifiersArray {
  // If modifiers already contains the required modifier, return it as is...
  if (modifiers !== undefined && hasModifier(modifiers, ensure)) {
    return modifiers;
  }
  // ...otherwise, add the modifier to the start of the array
  return ctx.factory.createNodeArray(
    [ctx.factory.createToken(ensure), ...(modifiers ?? [])],
    modifiers?.hasTrailingComma
  );
}

// Ensure a modifiers array doesn't have the specified modifier
export function ensureNoModifier(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined,
  ensure: ts.SyntaxKind.ExportKeyword | ts.SyntaxKind.DeclareKeyword
): ts.ModifiersArray {
  // If modifiers already doesn't contain the required modifier, return it as is...
  if (modifiers !== undefined && !hasModifier(modifiers, ensure)) {
    return modifiers;
  }
  // ...otherwise, remove the modifier
  return ctx.factory.createNodeArray(
    modifiers?.filter((m) => m.kind !== ensure),
    modifiers?.hasTrailingComma
  );
}

// Ensure a modifiers array includes the `export` modifier
export function ensureExportModifier(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined,
  exported = true
): ts.ModifiersArray {
  return exported
    ? ensureModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword)
    : ensureNoModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword);
}

// Ensures a modifiers array includes the `export declare` modifiers
export function ensureExportDeclareModifiers(
  ctx: ts.TransformationContext,
  modifiers: ts.ModifiersArray | undefined,
  exported = true
): ts.ModifiersArray {
  // Call in reverse, so we end up with `export declare` not `declare export`
  modifiers = ensureModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword);
  return ensureExportModifier(ctx, modifiers, exported);
}

// Make sure replacement node is `export`ed, with the `declare` modifier if it's
// a class, variable or function declaration.
// If the `noExport` option is set, only ensure `declare` modifiers
export function ensureStatementModifiers(
  ctx: ts.TransformationContext,
  node: ts.Node,
  exported = true
): ts.Node {
  if (ts.isClassDeclaration(node)) {
    return ctx.factory.updateClassDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers, exported),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isInterfaceDeclaration(node)) {
    return ctx.factory.updateInterfaceDeclaration(
      node,
      node.decorators,
      ensureExportModifier(
        ctx,
        exported
          ? ensureNoModifier(ctx, node.modifiers, ts.SyntaxKind.DeclareKeyword)
          : ensureModifier(ctx, node.modifiers, ts.SyntaxKind.DeclareKeyword),
        exported
      ),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isEnumDeclaration(node)) {
    return ctx.factory.updateEnumDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers, exported),
      node.name,
      node.members
    );
  }
  if (ts.isTypeAliasDeclaration(node)) {
    return ctx.factory.updateTypeAliasDeclaration(
      node,
      node.decorators,
      ensureExportModifier(
        ctx,
        exported
          ? ensureNoModifier(ctx, node.modifiers, ts.SyntaxKind.DeclareKeyword)
          : ensureModifier(ctx, node.modifiers, ts.SyntaxKind.DeclareKeyword),
        exported
      ),
      node.name,
      node.typeParameters,
      node.type
    );
  }
  if (ts.isVariableStatement(node)) {
    return ctx.factory.updateVariableStatement(
      node,
      ensureExportDeclareModifiers(ctx, node.modifiers, exported),
      node.declarationList
    );
  }
  if (ts.isFunctionDeclaration(node)) {
    return ctx.factory.updateFunctionDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers, exported),
      node.asteriskToken,
      node.name,
      node.typeParameters,
      node.parameters,
      node.type,
      node.body
    );
  }
  if (ts.isModuleDeclaration(node)) {
    return ctx.factory.updateModuleDeclaration(
      node,
      node.decorators,
      ensureExportDeclareModifiers(ctx, node.modifiers, exported),
      node.name,
      node.body
    );
  }
  assert.fail(`Expected statement, got "${printNode(node)}"`);
}
