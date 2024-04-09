// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import ts from "typescript";
import { printNode } from "../print";

// Checks whether the modifiers array contains a modifier of the specified kind
export function hasModifier(
  modifiers: ReadonlyArray<ts.Modifier> | undefined,
  kind: ts.Modifier["kind"]
): boolean {
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

export interface ModifierRequirements {
  export?: boolean;
  declare?: boolean;
}
// Ensures a ndoe satisfies the specified modifier requirements
function ensureModifierRequirements(
  ctx: ts.TransformationContext,
  node: ts.HasModifiers,
  reqs: ModifierRequirements
): ReadonlyArray<ts.Modifier> | undefined {
  let modifiers = ts.getModifiers(node);
  if (reqs.declare !== undefined) {
    modifiers = (reqs.declare ? ensureModifier : ensureNoModifier)(
      ctx,
      modifiers,
      ts.SyntaxKind.DeclareKeyword
    );
  }
  if (reqs.export !== undefined) {
    modifiers = (reqs.export ? ensureModifier : ensureNoModifier)(
      ctx,
      modifiers,
      ts.SyntaxKind.ExportKeyword
    );
  }
  return modifiers;
}

// Make sure replacement node is `export`ed, with the `declare` modifier if it's
// a class, variable or function declaration.
// If the `noExport` option is set, only ensure `declare` modifiers
export function ensureStatementModifiers(
  ctx: ts.TransformationContext,
  node: ts.Node,
  reqs: ModifierRequirements
): ts.Statement {
  if (ts.isClassDeclaration(node)) {
    return ctx.factory.updateClassDeclaration(
      node,
      ensureModifierRequirements(ctx, node, reqs),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isInterfaceDeclaration(node)) {
    return ctx.factory.updateInterfaceDeclaration(
      node,
      ensureModifierRequirements(ctx, node, { ...reqs, declare: undefined }),
      node.name,
      node.typeParameters,
      node.heritageClauses,
      node.members
    );
  }
  if (ts.isEnumDeclaration(node)) {
    return ctx.factory.updateEnumDeclaration(
      node,
      ensureModifierRequirements(ctx, node, reqs),
      node.name,
      node.members
    );
  }
  if (ts.isTypeAliasDeclaration(node)) {
    return ctx.factory.updateTypeAliasDeclaration(
      node,
      ensureModifierRequirements(ctx, node, { ...reqs, declare: undefined }),
      node.name,
      node.typeParameters,
      node.type
    );
  }
  if (ts.isVariableStatement(node)) {
    return ctx.factory.updateVariableStatement(
      node,
      ensureModifierRequirements(ctx, node, reqs),
      node.declarationList
    );
  }
  if (ts.isFunctionDeclaration(node)) {
    return ctx.factory.updateFunctionDeclaration(
      node,
      ensureModifierRequirements(ctx, node, reqs),
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
      ensureModifierRequirements(ctx, node, reqs),
      node.name,
      node.body
    );
  }
  if (
    ts.isImportDeclaration(node) ||
    ts.isImportEqualsDeclaration(node) ||
    ts.isExportDeclaration(node) ||
    ts.isExportAssignment(node)
  ) {
    return node;
  }
  assert.fail(`Expected statement, got "${printNode(node)}"`);
}
