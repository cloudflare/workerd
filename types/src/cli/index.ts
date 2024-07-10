#!/usr/bin/env node
import assert from "node:assert";
import fs from "node:fs/promises";
import path from "node:path";
import util from "node:util";
import ts from "typescript";
import {
  CommentsData,
  createMemoryProgram,
  installComments,
} from "../";

async function collateStandardComments() {
  const typeScriptPath = path.dirname(require.resolve("typescript"));
  // TODO(someday): collate types from `@types/node` too
  const libPath = path.join(typeScriptPath, "lib.webworker.d.ts");
  const libContents = (await fs.readFile(libPath, "utf8")).split(
    // Remove copyright notices to prevent them being copied in as comments
    "/////////////////////////////"
  )[2];
  const libIterablePath = path.join(typeScriptPath, "lib.webworker.iterable.d.ts");
  const libIterableContents = (await fs.readFile(libPath, "utf8")).split(
    // Remove copyright notices to prevent them being copied in as comments
    "/////////////////////////////"
  )[2];
  const program = createMemoryProgram(
    new Map([
      [libPath, libContents],
      [libIterablePath, libIterableContents]
    ])
  );
  const libFile = program.getSourceFile(libPath);
  assert(libFile !== undefined);

  const result: CommentsData = {};
  const recordComments = (node: ts.Node, name: string, memberName?: string) => {
    const ranges = ts.getLeadingCommentRanges(libContents, node.getFullStart());
    if (ranges === undefined) return;

    const nodeResult = (result[name] ??= {});
    const key = memberName ?? "$";
    nodeResult[key] ??= "";

    for (const range of ranges) {
      let text =
        range.kind === ts.SyntaxKind.MultiLineCommentTrivia
          ? libContents.slice(range.pos + 2, range.end - 2)
          : libContents.slice(range.pos + 2, range.end);
      text = text.replaceAll(/^\s+/gm, " "); // Strip excess indentation
      nodeResult[key] += text;
    }
  };

  ts.forEachChild(libFile, (node) => {
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

// Options:
//  --comments-output-path
//    `.json` file to write containing collated comments data
export async function main(args?: string[]) {
  const { values: opts } = util.parseArgs({
    options: {
      "comments-output-path": { type: "string" },
    },
    strict: true,
    allowPositionals: false,
    args,
  });

  if (opts["comments-output-path"] === undefined) {
    throw new Error(
      "Expected --comments-output-path to be defined"
    );
  }

  const comments = await collateStandardComments();
  const commentsOutputPath = opts["comments-output-path"];
  installComments(comments);
  if (commentsOutputPath !== undefined) {
    await fs.mkdir(path.dirname(commentsOutputPath), { recursive: true });
    await fs.writeFile(commentsOutputPath, JSON.stringify(comments));
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
