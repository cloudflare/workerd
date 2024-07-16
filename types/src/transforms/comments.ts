// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import ts from "typescript";
import { hasModifier } from "./helpers";

// Refer to `collateStandardComments()` in `standards.ts` for construction
export type CommentsData = Record<
  /* globalName */ string,
  | Record</* root */ "$" | /* memberName */ string, string | undefined>
  | undefined
>;

export function createCommentsTransformer(
  comments: CommentsData
): ts.TransformerFactory<ts.SourceFile> {
  return (ctx) => {
    return (node) => {
      const visitor = createVisitor(comments);
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
