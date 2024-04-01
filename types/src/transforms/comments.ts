import ts from "typescript";
import { hasModifier } from "./helpers";

// Refer to `collateStandardComments()` in `src/cli/index.ts` for construction
export type CommentsData = Record<
  /* globalName */ string,
  | Record</* root */ "$" | /* memberName */ string, string | undefined>
  | undefined
>;

let installedData: CommentsData | undefined;
export function installComments(newData: CommentsData) {
  installedData = newData;
}

export function createCommentsTransformer(): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      if (installedData === undefined) return node;
      const visitor = createVisitor(installedData);
      return ts.visitEachChild(node, visitor, ctx);
    };
  };
}

function maybeAddComment(node: ts.Node, comment?: string) {
  if (comment === undefined) return;
  ts.addSyntheticLeadingComment(
    node,
    ts.SyntaxKind.MultiLineCommentTrivia,
    comment,
    /* hasTrailingNewLine */ true
  );
}

function createVisitor(data: CommentsData) {
  return (node: ts.Node) => {
    if (
      (ts.isClassDeclaration(node) || ts.isInterfaceDeclaration(node)) &&
      node.name !== undefined &&
      ts.isIdentifier(node.name)
    ) {
      const comments = data[node.name.text];
      maybeAddComment(node, comments?.["$"]);
      for (const member of node.members) {
        if (member.name !== undefined && ts.isIdentifier(member.name)) {
          let key = member.name.text;

          const isStatic =
            ts.canHaveModifiers(member) &&
            hasModifier(ts.getModifiers(member), ts.SyntaxKind.StaticKeyword);
          if (isStatic) key = `static:${key}`;

          maybeAddComment(member, comments?.[key]);
        }
      }
    }

    if (
      ts.isFunctionDeclaration(node) &&
      node.name !== undefined &&
      ts.isIdentifier(node.name)
    ) {
      const comments = data[node.name.text];
      maybeAddComment(node, comments?.["$"]);
    }

    if (
      ts.isVariableStatement(node) &&
      node.declarationList.declarations.length === 1
    ) {
      const declaration = node.declarationList.declarations[0];
      const name = declaration.name;
      if (ts.isIdentifier(name)) {
        const comments = data[name.text];
        maybeAddComment(node, comments?.["$"]);
      }
    }

    return node;
  };
}
