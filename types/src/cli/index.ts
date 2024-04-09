#!/usr/bin/env node
import assert from "node:assert";
import fs from "node:fs/promises";
import path from "node:path";
import util from "node:util";
import ts from "typescript";
import {
  CommentsData,
  buildTypes,
  createMemoryProgram,
  installComments,
  makeImportable,
} from "../";

async function* walkDir(root: string): AsyncGenerator<string> {
  const entries = await fs.readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    const entryPath = path.join(root, entry.name);
    if (entry.isDirectory()) yield* walkDir(entryPath);
    else yield entryPath;
  }
}

async function collateExtraDefinitions(definitionsDir?: string) {
  if (definitionsDir === undefined) return "";
  const files: Promise<string>[] = [];
  for await (const filePath of walkDir(path.resolve(definitionsDir))) {
    files.push(fs.readFile(filePath, "utf8"));
  }
  return (await Promise.all(files)).join("\n");
}

async function collateStandardComments() {
  const typeScriptPath = path.dirname(require.resolve("typescript"));
  // TODO(someday): collate types from `@types/node` too
  const libPath = path.join(typeScriptPath, "lib.webworker.d.ts");
  const libContents = (await fs.readFile(libPath, "utf8")).split(
    // Remove copyright notices to prevent them being copied in as comments
    "/////////////////////////////"
  )[2];
  const program = createMemoryProgram(new Map([[libPath, libContents]]));
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

function checkAndGetDiagnostics(sources: Map<string, string>): string[] {
  const compilerOpts: ts.CompilerOptions = {
    noEmit: true,
    lib: ["lib.esnext.d.ts"],
    types: [], // Explicitly ignore `@types/node` from dependencies
  };
  const host = ts.createCompilerHost(compilerOpts, /* setParentNodes */ true);

  const originalFileExists = host.fileExists;
  const originalReadFile = host.readFile;

  host.getDefaultLibLocation = () =>
    path.dirname(require.resolve("typescript"));
  host.fileExists = (fileName: string) => {
    if (sources.has(fileName)) return true;
    return originalFileExists.call(host, fileName);
  };
  host.readFile = (filePath: string) => {
    const maybeSource = sources.get(filePath);
    if (maybeSource !== undefined) return maybeSource;
    return originalReadFile.call(host, filePath);
  };

  const program = createMemoryProgram(sources, host, compilerOpts);

  const emitResult = program.emit();
  const allDiagnostics = ts
    .getPreEmitDiagnostics(program)
    .concat(emitResult.diagnostics);

  return allDiagnostics.map((diagnostic) => {
    if (diagnostic.file) {
      const { line, character } = ts.getLineAndCharacterOfPosition(
        diagnostic.file,
        diagnostic.start!
      );
      const message = ts.flattenDiagnosticMessageText(
        diagnostic.messageText,
        "\n"
      );
      return `${diagnostic.file.fileName}:${line + 1}:${character + 1}: ${message}`;
    } else {
      return ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n");
    }
  });
}

// Generates TypeScript types from a binary Cap’n Proto file containing encoded
// JSG RTTI. See src/workerd/tools/api-encoder.c++ for a script that generates
// input expected by this tool.
//
// To generate types using default options, run `bazel build //types:types`.
//
// Usage: types [options]
//
// Options:
//  --rtti-capnp-path <path>
//    File of binary Cap'n Proto encoded JSG RTTI to generate types from
//  --defines-path <path>
//    Directory of extra TypeScript definitions, not associated with C++ files,
//    to concatenate to the output
//  --output-path
//    `.d.ts` file to write generated types to
//  --importable
//    Whether to produce types that can be imported instead of ambient ones
//  --comments-output-path
//    `.json` file to write containing collated comments data
export async function main(args?: string[]) {
  const { values: opts } = util.parseArgs({
    options: {
      "rtti-capnp-path": { type: "string" },
      "defines-path": { type: "string" },
      "params-names-json-path": { type: "string" },
      "output-path": { type: "string" },
      importable: { type: "boolean" },
      "comments-output-path": { type: "string" },
    },
    strict: true,
    allowPositionals: false,
    args,
  });

  if (opts["rtti-capnp-path"] === undefined) {
    throw new Error(
      "Expected --rtti-capnp-path to point to a binary Cap'n Proto file containing runtime-type-information"
    );
  }

  const comments = await collateStandardComments();
  const commentsOutputPath = opts["comments-output-path"];
  installComments(comments);
  if (commentsOutputPath !== undefined) {
    await fs.mkdir(path.dirname(commentsOutputPath), { recursive: true });
    await fs.writeFile(commentsOutputPath, JSON.stringify(comments));
  }

  const rttiCapnpBuffer = await fs.readFile(opts["rtti-capnp-path"]);
  const extraDefinitions = await collateExtraDefinitions(opts["defines-path"]);
  let definitions = buildTypes({ rttiCapnpBuffer, extraDefinitions });
  if (opts.importable) definitions = makeImportable(definitions);

  const sources = new Map([["/$virtual/source.ts", definitions]]);
  const diagnostics = checkAndGetDiagnostics(sources);
  if (diagnostics.length > 0) {
    for (const diagnostic of diagnostics) console.error(diagnostic);
    process.exitCode = 1;
    return;
  }

  const outputPath = opts["output-path"];
  if (outputPath === undefined) {
    process.stdout.write(definitions);
  } else {
    await fs.mkdir(path.dirname(outputPath), { recursive: true });
    await fs.writeFile(outputPath, definitions);
  }
}

// Outputting to a CommonJS module so can't use top-level await
if (require.main === module) void main();
