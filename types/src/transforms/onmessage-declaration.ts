import ts from "typescript";

export function createAddOnMessageDeclarationTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (context) => {
    return (sourceFile) => {
      // Create the new variable declaration
      const onMessageDeclaration = context.factory.createVariableStatement(
        [context.factory.createModifier(ts.SyntaxKind.DeclareKeyword)],
        context.factory.createVariableDeclarationList(
          [
            context.factory.createVariableDeclaration(
              "onmessage",
              undefined,
              context.factory.createKeywordTypeNode(ts.SyntaxKind.NeverKeyword)
            ),
          ],
          ts.NodeFlags.None
        )
      );

      // Append the new declaration to the source file
      const updatedStatements = ts.factory.createNodeArray([
        onMessageDeclaration,
        ...sourceFile.statements,
      ]);

      // Return the updated source file
      return ts.factory.updateSourceFile(sourceFile, updatedStatements);
    };
  };
}
