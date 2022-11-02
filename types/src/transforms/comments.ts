import assert from "assert";
import { ParsedTypeDefinition } from "src";
import ts from "typescript";

function attachComments(
  from: ts.Node,
  to: ts.Node,
  sourceFile: ts.SourceFile
): void {
  const text = sourceFile.getFullText();
  const extractCommentText = (comment: ts.CommentRange) =>
    comment.kind === ts.SyntaxKind.MultiLineCommentTrivia
      ? text.slice(comment.pos + 2, comment.end - 2)
      : text.slice(comment.pos + 2, comment.end);
  const leadingComments: string[] =
    ts
      .getLeadingCommentRanges(text, from.getFullStart())
      ?.map(extractCommentText) ?? [];

  leadingComments.map((c) =>
    ts.addSyntheticLeadingComment(
      to,
      ts.SyntaxKind.MultiLineCommentTrivia,
      c,
      true
    )
  );
}

export function createCommentsTransformer(
  standards: ParsedTypeDefinition
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(standards);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

export function createVisitor(standards: ParsedTypeDefinition) {
  return (node: ts.Node) => {
    if (ts.isClassDeclaration(node)) {
      const name = node.name?.getText();
      assert(name !== undefined);
      const standardsVersion = standards.parsed.vars.get(name);
      if (standardsVersion) {
        const type = standardsVersion.type;
        assert(
          ts.isTypeLiteralNode(type!),
          `Non type literal found for "${name}"`
        );
        attachComments(standardsVersion, node, standards.source);
        const standardsInterface = standards.parsed.interfaces.get(name);
        assert(standardsInterface !== undefined);
        node.members.forEach((member) => {
          const n = member.name?.getText();
          if (!n && ts.isConstructorDeclaration(member)) return;

          assert(n !== undefined, `No name for child of ${name}`);
          let standardsEquivalent;
          if (
            member.modifiers?.find(
              (m) => m.kind === ts.SyntaxKind.StaticKeyword
            )
          ) {
            standardsEquivalent = type.members.find(
              (m) => m.name?.getText() === n
            );
          } else {
            standardsEquivalent = standardsInterface.members.find(
              (m) => m.name?.getText() === n
            );
          }
          if (standardsEquivalent)
            attachComments(standardsEquivalent, member, standards.source);
        });
      }
    } else if (ts.isFunctionDeclaration(node)) {
      const name = node.name?.getText();
      assert(name !== undefined);
      const standardsVersion = standards.parsed.functions.get(name);
      if (standardsVersion) {
        attachComments(standardsVersion, node, standards.source);
      }
    } else if (ts.isInterfaceDeclaration(node)) {
      const name = node.name?.getText();
      assert(name !== undefined);
      const standardsVersion = standards.parsed.interfaces.get(name);
      if (standardsVersion) {
        attachComments(standardsVersion, node, standards.source);
        node.members.forEach((member) => {
          const n = member.name?.getText();
          assert(n !== undefined);
          const standardsEquivalent = standardsVersion.members.find(
            (m) => m.name?.getText() === n
          );
          if (standardsEquivalent)
            attachComments(standardsEquivalent, member, standards.source);
        });
      }
    } else if (ts.isTypeAliasDeclaration(node)) {
      const name = node.name?.getText();
      assert(name !== undefined);
      const standardsVersion = standards.parsed.types.get(name);
      if (standardsVersion) {
        attachComments(standardsVersion, node, standards.source);
      }
    } else if (ts.isVariableStatement(node)) {
      const ignoreForComments = new Set(["self", "navigator", "caches"]);

      const name = node.declarationList.declarations[0].name.getText();
      assert(node.declarationList.declarations.length === 1);
      assert(name !== undefined);
      const standardsVersion = standards.parsed.vars.get(name);
      if (standardsVersion && !ignoreForComments.has(name)) {
        attachComments(standardsVersion, node, standards.source);
      }
    }

    return node;
  };
}
