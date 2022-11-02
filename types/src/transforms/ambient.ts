import ts from "typescript";

export function createAmbientTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor();
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

export function createVisitor() {
  return (node: ts.Node) => {
    // Filter out export keywords
    // @ts-ignore next-line
    node.modifiers = node.modifiers?.filter(
      (m) => m.kind !== ts.SyntaxKind.ExportKeyword
    );
    // Ensure every node has a declare keyword
    if (!node.modifiers?.find((m) => m.kind == ts.SyntaxKind.DeclareKeyword)) {
      // @ts-ignore next-line
      node.modifiers = [
        ts.factory.createModifier(ts.SyntaxKind.DeclareKeyword),
        ...(node.modifiers ?? []),
      ];
    }

    return node;
  };
}
