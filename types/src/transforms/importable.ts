import ts from "typescript";
import { ensureStatementModifiers } from "./helpers";

// This ensures that all nodes have the `export` keyword (and, where relevant, `export declare`)
export function createImportableTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(ctx);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function createVisitor(ctx: ts.TransformationContext) {
  return (node: ts.Node) => {
    return ensureStatementModifiers(ctx, node);
  };
}
