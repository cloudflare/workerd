// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import ts from "typescript";
import { ensureStatementModifiers } from "./helpers";

// This ensures that all top-level nodes are `export`ed, and removes
// `declare module` blocks.
//
// ```ts
// declare class A { ... }
// declare module "thing" {
//   class B { ... }
// }
// ```
//
// --- transforms to --->
//
// ```ts
// export declare class A { ... }
// ```
export function createImportableTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function createVisitor(ctx: ts.TransformationContext): ts.Visitor {
  return (node: ts.Node) => {
    // Remove `module` declarations (e.g. `declare module "assets:*" {...}`) as
    // these can't be `export`ed, and don't really make sense in non-ambient
    // declarations
    if (
      ts.isModuleDeclaration(node) &&
      (node.flags & ts.NodeFlags.Namespace) === 0
    ) {
      return;
    }
    return ensureStatementModifiers(ctx, node, { export: true });
  };
}
