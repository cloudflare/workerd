import { assert } from "console";
import { readFile } from "fs/promises";
import * as ts from "typescript";
import { createMemoryProgram } from "./program";
export interface ParsedTypeDefinition {
  program: ts.Program;
  source: ts.SourceFile;
  checker: ts.TypeChecker;
  parsed: {
    functions: Map<string, ts.FunctionDeclaration>;
    interfaces: Map<string, ts.InterfaceDeclaration>;
    vars: Map<string, ts.VariableDeclaration>;
    types: Map<string, ts.TypeAliasDeclaration>;
    classes: Map<string, ts.ClassDeclaration>;
  };
}

// Collate standards (to support lib.(dom|webworker).iterable.d.ts being defined separately)
export async function collateStandards(
  ...standardTypes: string[]
): Promise<ParsedTypeDefinition> {
  const STANDARDS_PATH = "/source.ts";
  const text: string = (
    await Promise.all(
      standardTypes.map(
        async (s) =>
          // Remove the Microsoft copyright notices from the file, to prevent them being copied in as TS comments
          (await readFile(s, "utf-8")).split(`/////////////////////////////`)[2]
      )
    )
  ).join("\n");
  const program = createMemoryProgram(new Map([[STANDARDS_PATH, text]]));
  const source = program.getSourceFile(STANDARDS_PATH)!;
  const checker = program.getTypeChecker();
  const parsed = {
    functions: new Map<string, ts.FunctionDeclaration>(),
    interfaces: new Map<string, ts.InterfaceDeclaration>(),
    vars: new Map<string, ts.VariableDeclaration>(),
    types: new Map<string, ts.TypeAliasDeclaration>(),
    classes: new Map<string, ts.ClassDeclaration>(),
  };
  ts.forEachChild(source, (node) => {
    let name = "";
    if (node && ts.isFunctionDeclaration(node)) {
      name = node.name?.text ?? "";
      parsed.functions.set(name, node);
    } else if (ts.isVariableStatement(node)) {
      assert(node.declarationList.declarations.length === 1);
      name = node.declarationList.declarations[0].name.getText(source);
      parsed.vars.set(name, node.declarationList.declarations[0]);
    } else if (ts.isInterfaceDeclaration(node)) {
      name = node.name.text;
      parsed.interfaces.set(name, node);
    } else if (ts.isTypeAliasDeclaration(node)) {
      name = node.name.text;
      parsed.types.set(name, node);
    } else if (ts.isClassDeclaration(node)) {
      name = node.name?.text ?? "";
      parsed.classes.set(name, node);
    }
  });
  return {
    program,
    source,
    checker,
    parsed,
  };
}
