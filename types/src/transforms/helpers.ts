// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "assert";
import * as ts from "typescript";
import { printNode } from "../print";
// Checks whether the modifiers array contains a modifier of the specified kind
export function hasModifier(
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  kind: ts.Modifier["kind"]
) {
  if (modifiers === undefined) return false;
  return modifiers.some((modifier) => modifier.kind === kind);
}

// Ensure a modifiers array has the specified modifier, inserting it at the
// start if it doesn't.
export function ensureModifier(
  ctx: ts.TransformationContext,
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  ensure: ts.SyntaxKind.ExportKeyword | ts.SyntaxKind.DeclareKeyword
): ReadonlyArray<ts.Modifier> {
  // If modifiers already contains the required modifier, return it as is...
  if (modifiers !== undefined && hasModifier(modifiers, ensure)) {
    return modifiers;
  }
  // ...otherwise, add the modifier to the start of the array
  return [ctx.factory.createToken(ensure), ...(modifiers ?? [])];
}

// Ensure a modifiers array doesn't have the specified modifier
export function ensureNoModifier(
  ctx: ts.TransformationContext,
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  ensure: ts.SyntaxKind.ExportKeyword | ts.SyntaxKind.DeclareKeyword
): ReadonlyArray<ts.Modifier> {
  // If modifiers already doesn't contain the required modifier, return it as is...
  if (modifiers !== undefined && !hasModifier(modifiers, ensure)) {
    return modifiers;
  }
  // ...otherwise, remove the modifier
  return modifiers?.filter((m) => m.kind !== ensure) ?? [];
}

// Ensure a modifiers array includes the `export` modifier
export function ensureExportModifier(
  ctx: ts.TransformationContext,
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  exported = true
): ReadonlyArray<ts.Modifier> {
  return exported
    ? ensureModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword)
    : ensureNoModifier(ctx, modifiers, ts.SyntaxKind.ExportKeyword);
}

// Ensures a modifiers array includes the `export declare` modifiers
export function ensureExportDeclareModifiers(
  ctx: ts.TransformationContext,
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  exported = true
): ReadonlyArray<ts.Modifier> {
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
      ensureExportDeclareModifiers(ctx, ts.getModifiers(node), exported),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isInterfaceDeclaration(node)) {
    const modifiers = ts.getModifiers(node);
    return ctx.factory.updateInterfaceDeclaration(
      node,
      ensureExportModifier(
        ctx,
        exported
          ? ensureNoModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword)
          : ensureModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword),
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
      ensureExportDeclareModifiers(ctx, ts.getModifiers(node), exported),
      node.name,
      node.members
    );
  }
  if (ts.isTypeAliasDeclaration(node)) {
    const modifiers = ts.getModifiers(node);
    return ctx.factory.updateTypeAliasDeclaration(
      node,
      ensureExportModifier(
        ctx,
        exported
          ? ensureNoModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword)
          : ensureModifier(ctx, modifiers, ts.SyntaxKind.DeclareKeyword),
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
      ensureExportDeclareModifiers(ctx, ts.getModifiers(node), exported),
      node.declarationList
    );
  }
  if (ts.isFunctionDeclaration(node)) {
    return ctx.factory.updateFunctionDeclaration(
      node,
      ensureExportDeclareModifiers(ctx, ts.getModifiers(node), exported),
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
      ensureExportDeclareModifiers(ctx, ts.getModifiers(node), exported),
      node.name,
      node.body
    );
  }
  assert.fail(`Expected statement, got "${printNode(node)}"`);
}
