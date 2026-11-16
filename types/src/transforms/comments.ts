// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import ts from "typescript";
import { hasModifier } from "./helpers";

// Refer to `collateStandardComments()` in `standards.ts` for construction
export type CommentsData = Record<
  /* globalName */ string,
  // eslint-disable-next-line @typescript-eslint/no-redundant-type-constituents
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

function maybeAddComment(node: ts.Node, comment?: string): void {
  if (comment === undefined) return;
  ts.addSyntheticLeadingComment(
    node,
    ts.SyntaxKind.MultiLineCommentTrivia,
    comment,
    /* hasTrailingNewLine */ true
  );
}

// `record[key]` if `record` has it as its own property. The comments data
// are plain objects, whose prototype has members such as `toString`, which
// a member without a comment of its own must not find.
function lookup<T>(
  record: Record<string, T> | undefined,
  key: string
): T | undefined {
  return record !== undefined && Object.hasOwn(record, key)
    ? record[key]
    : undefined;
}

function createVisitor(data: CommentsData): ts.Visitor {
  return (node) => {
    if (
      (ts.isClassDeclaration(node) || ts.isInterfaceDeclaration(node)) &&
      node.name !== undefined &&
      ts.isIdentifier(node.name)
    ) {
      const comments = lookup(data, node.name.text);
      maybeAddComment(node, lookup(comments, "$"));
      for (const member of node.members) {
        if (member.name !== undefined && ts.isIdentifier(member.name)) {
          let key = member.name.text;

          const isStatic =
            ts.canHaveModifiers(member) &&
            hasModifier(ts.getModifiers(member), ts.SyntaxKind.StaticKeyword);
          if (isStatic) key = `static:${key}`;

          maybeAddComment(member, lookup(comments, key));
        }
      }
    }

    if (
      ts.isFunctionDeclaration(node) &&
      node.name !== undefined &&
      ts.isIdentifier(node.name)
    ) {
      const comments = lookup(data, node.name.text);
      maybeAddComment(node, lookup(comments, "$"));
    }

    if (
      ts.isVariableStatement(node) &&
      node.declarationList.declarations.length === 1
    ) {
      const declaration = node.declarationList.declarations[0];
      const name = declaration.name;
      if (ts.isIdentifier(name)) {
        const comments = lookup(data, name.text);
        maybeAddComment(node, lookup(comments, "$"));
      }
    }

    return node;
  };
}
