import path from "path";
import ts from "typescript";

interface MemorySourceFile {
  source: string;
  sourceFile: ts.SourceFile;
}

// Creates a TypeScript program from in-memory source files. Accepts a Map of
// fully-resolved "virtual" paths to source code.
export function createMemoryProgram(sources: Map<string, string>): ts.Program {
  const options = ts.getDefaultCompilerOptions();
  const host = ts.createCompilerHost(options, true);

  const sourceFiles = new Map<string, MemorySourceFile>();
  for (const [sourcePath, source] of sources) {
    const sourceFile = ts.createSourceFile(
      sourcePath,
      source,
      ts.ScriptTarget.ESNext,
      false,
      ts.ScriptKind.TS
    );
    sourceFiles.set(sourcePath, { source, sourceFile });
  }

  // Update compiler host to return in-memory source file
  function patchHostMethod<
    K extends "fileExists" | "readFile" | "getSourceFile"
  >(
    key: K,
    placeholderResult: (f: MemorySourceFile) => ReturnType<ts.CompilerHost[K]>
  ) {
    const originalMethod: (...args: any[]) => any = host[key];
    host[key] = (fileName: string, ...args: any[]) => {
      const sourceFile = sourceFiles.get(path.resolve(fileName));
      if (sourceFile !== undefined) {
        return placeholderResult(sourceFile);
      }
      return originalMethod.call(host, fileName, ...args);
    };
  }
  patchHostMethod("fileExists", () => true);
  patchHostMethod("readFile", ({ source }) => source);
  patchHostMethod("getSourceFile", ({ sourceFile }) => sourceFile);

  const rootNames = [...sourceFiles.keys()];
  return ts.createProgram(rootNames, options, host);
}
