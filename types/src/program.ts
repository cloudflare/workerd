// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";
import ts from "typescript";

export const SourcesMap = Map<string, string>;
export type SourcesMap = InstanceType<typeof SourcesMap>;

const defaultCompilerOpts = ts.getDefaultCompilerOptions();

// Creates a TypeScript program from in-memory source files. Accepts a Map of
// fully-resolved "virtual" paths to source code.
export function createMemoryProgram(
  sources: SourcesMap,
  host?: ts.CompilerHost,
  compilerOpts = defaultCompilerOpts,
  // Provide additional lib files to TypeScript. These should all be prefixed with
  // `/node_modules/typescript/lib` (e.g. `/node_modules/typescript/lib/lib.esnext.d.ts`)
  lib?: SourcesMap
): ts.Program {
  const sourceFiles = new Map<string, ts.SourceFile>();
  for (const [sourcePath, source] of [...sources, ...(lib ?? [])]) {
    const sourceFile = ts.createSourceFile(
      sourcePath,
      source,
      ts.ScriptTarget.ESNext,
      false,
      ts.ScriptKind.TS
    );
    sourceFiles.set(sourcePath, sourceFile);
  }

  host ??= {
    getCurrentDirectory(): string {
      return "/";
    },
    getCanonicalFileName(fileName: string): string {
      return fileName;
    },
    getDefaultLibFileName(_options: ts.CompilerOptions): string {
      return "";
    },
    getDefaultLibLocation() {
      return "/node_modules/typescript/lib";
    },
    getNewLine(): string {
      return "\n";
    },
    useCaseSensitiveFileNames(): boolean {
      return true;
    },
    fileExists(fileName: string): boolean {
      return sourceFiles.has(fileName);
    },
    readFile(_path: string): string | undefined {
      assert.fail("readFile() not implemented");
    },
    writeFile(
      _path: string,
      _data: string,
      _writeByteOrderMark?: boolean
    ): void {
      assert.fail("writeFile() not implemented");
    },
    getSourceFile(
      fileName: string,
      _languageVersionOrOptions: ts.ScriptTarget | ts.CreateSourceFileOptions,
      _onError?: (message: string) => void,
      _shouldCreateNewSourceFile?: boolean
    ): ts.SourceFile | undefined {
      return sourceFiles.get(fileName);
    },
  };

  const rootNames = Array.from(sources.keys());
  return ts.createProgram(rootNames, compilerOpts, host);
}
