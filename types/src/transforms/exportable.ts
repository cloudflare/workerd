import ts from "typescript";

export function createExportableTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor();
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

export function createVisitor() {
  return (node: ts.Node) => {
    // Ensure export keywords
    if (!node.modifiers?.find((m) => m.kind == ts.SyntaxKind.ExportKeyword)) {
      // @ts-ignore next-line
      node.modifiers = [
        ts.factory.createModifier(ts.SyntaxKind.ExportKeyword),
        ...(node.modifiers ?? []),
      ];
    }
    // Filter declare keywords for interfaces
    if (ts.isInterfaceDeclaration(node)) {
      // @ts-ignore next-line
      node.modifiers = node.modifiers?.filter(
        (m) => m.kind !== ts.SyntaxKind.DeclareKeyword
      );
    }

    return node;
  };
}
