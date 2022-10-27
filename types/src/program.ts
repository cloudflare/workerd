import path from "path";
import ts from "typescript";

export function createMemoryProgram(source: string): [ts.Program, string] {
  const options = ts.getDefaultCompilerOptions();
  const host = ts.createCompilerHost(options, true);

  const sourcePath = path.resolve(__dirname, "source.ts");
  const sourceFile = ts.createSourceFile(
    sourcePath,
    source,
    ts.ScriptTarget.ESNext,
    false,
    ts.ScriptKind.TS
  );

  // Update compiler host to return in-memory source file
  function patchHostMethod<
    K extends "fileExists" | "readFile" | "getSourceFile"
  >(key: K, placeholderResult: ReturnType<ts.CompilerHost[K]>) {
    const originalMethod: (...args: any[]) => any = host[key];
    host[key] = (fileName: string, ...args: any[]) => {
      if (path.resolve(fileName) === sourcePath) {
        return placeholderResult;
      }
      return originalMethod.call(host, fileName, ...args);
    };
  }
  patchHostMethod("fileExists", true);
  patchHostMethod("readFile", source);
  patchHostMethod("getSourceFile", sourceFile);

  const program = ts.createProgram([sourcePath], options, host);
  return [program, sourcePath];
}
