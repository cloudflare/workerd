// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import ts from "typescript";
import { ensureStatementModifiers } from "./helpers";

// This ensures that all nodes have the `declare` keyword
export function createAmbientTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function createVisitor(ctx: ts.TransformationContext) {
  return (node: ts.Node) => {
    return ensureStatementModifiers(ctx, node, false);
  };
}
