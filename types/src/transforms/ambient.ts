// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import ts from "typescript";
import { ensureStatementModifiers } from "./helpers";

// This ensures that all top-level nodes have the `declare` keyword, but no
// nodes inside `declare modules` blocks do. It also ensures no top-level
// nodes are `export`ed.
//
// ```ts
// export class A { ... }
// declare module "thing" {
//   declare class B { ... }
// }
// ```
//
// --- transforms to --->
//
// ```ts
// declare class A { ... }
// declare module "thing" {
//   class B { ... }
// }
// ```
export function createAmbientTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function createVisitor(ctx: ts.TransformationContext): ts.Visitor {
  const moduleBlockChildVisitor: ts.Visitor = (node) => {
    return ensureStatementModifiers(ctx, node, { declare: false });
  };
  const moduleDeclarationVisitor: ts.Visitor = (node) => {
    if (ts.isModuleBlock(node)) {
      return ts.visitEachChild(node, moduleBlockChildVisitor, ctx);
    }
    return node;
  };
  return (node) => {
    if (
      ts.isModuleDeclaration(node) &&
      (node.flags & ts.NodeFlags.Namespace) === 0
    ) {
      return ts.visitEachChild(node, moduleDeclarationVisitor, ctx);
    }
    return ensureStatementModifiers(ctx, node, {
      export: false,
      declare: true,
    });
  };
}
