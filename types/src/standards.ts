import assert from "node:assert";
import { readFileSync } from "node:fs";
import * as ts from "typescript";
import { createMemoryProgram } from "./program";
import { CommentsData } from "./transforms";

// Collate comments from standards-based .d.ts files (e.g. lib.webworker.d.ts)
export function collateStandardComments(
  ...standardTypes: string[]
): CommentsData {
  const combinedLibPath = "/$virtual/standards.d.ts";
  const combinedLibContents: string = standardTypes
    .map(
      (s) =>
        // Remove the Microsoft copyright notices from the file, to prevent them being copied in as TS comments
        readFileSync(s, "utf-8").split(`/////////////////////////////`)[2]
    )
    .join("\n");

  const program = createMemoryProgram(
    new Map([[combinedLibPath, combinedLibContents]])
  );

  const combinedLibFile = program.getSourceFile(combinedLibPath);

  assert(combinedLibFile !== undefined);

  const result: CommentsData = {};
  const recordComments = (node: ts.Node, name: string, memberName?: string) => {
    const ranges = ts.getLeadingCommentRanges(
      combinedLibContents,
      node.getFullStart()
    );
    if (ranges === undefined) return;

    const nodeResult = (result[name] ??= {});
    const key = memberName ?? "$";
    nodeResult[key] ??= "";

    for (const range of ranges) {
      let text = combinedLibContents.slice(range.pos + 2, range.end);
      // All lib.*.d.ts file use multiline comment syntax (/** ... */).
      // For the avoidance of doubt and to make sure the following parsing code is valid,
      // assert that only multiline comments are included.
      assert(
        range.kind === ts.SyntaxKind.MultiLineCommentTrivia,
        "Unexpected single-line comment in a standards .d.ts file"
      );
      text = text
        // Remove the multiline comment end
        .slice(0, text.length - 2)
        // Remove multiline comment prefix lines (e.g. `   * ` -> ` * `)
        .replaceAll(/^\s+/gm, " ");

      // Let's make sure comments that are actually only 1 line (usually a link to MDN) don't start
      // with two * characters (e.g. `/** [MDN Reference] ... */` -> `/* [MDN Reference] ... */`)
      if (!text.includes("\n") && text.startsWith("*")) {
        text = text.slice(1);
      }

      // Because we load multiple lib files, some types are included multiple times.
      // This simple check makes sure that we don't add a doc comment twice
      if (!nodeResult[key]?.includes?.(text)) {
        nodeResult[key] += text;
      }
    }
  };

  ts.forEachChild(combinedLibFile, (node) => {
    if (ts.isInterfaceDeclaration(node)) {
      // Prototype properties/methods exist on interfaces, for example:
      // ```ts
      // /** ... */
      // interface AbortSignal extends EventTarget {
      //   /** ... */
      //   readonly aborted: boolean;
      // }
      // ```
      recordComments(node, node.name.text);
      for (const member of node.members) {
        if (member.name === undefined) continue;
        if (!ts.isIdentifier(member.name)) continue;
        recordComments(member, node.name.text, member.name.text);
      }
    } else if (ts.isFunctionDeclaration(node)) {
      if (node.name !== undefined) recordComments(node, node.name.text);
    } else if (ts.isVariableStatement(node)) {
      if (node.declarationList.declarations.length > 1) return;
      const declaration = node.declarationList.declarations[0];
      const name = declaration.name;
      if (!ts.isIdentifier(name)) return;
      recordComments(node, name.text);

      if (
        declaration.type !== undefined &&
        ts.isTypeLiteralNode(declaration.type)
      ) {
        // Static properties/methods exist on type literals, for example:
        // ```ts
        // declare var AbortSignal: {
        //   prototype: AbortSignal;
        //   new(): AbortSignal;
        //   /** ... */
        //   abort(reason?: any): AbortSignal;
        // };
        // ```
        for (const member of declaration.type.members) {
          if (member.name === undefined) continue;
          if (!ts.isIdentifier(member.name)) continue;
          recordComments(member, name.text, `static:${member.name.text}`);
        }
      }
    }
  });

  return result;
}
