import assert from "assert";
import { test } from "node:test";
import ts, { factory as f } from "typescript";
import { printNode, printNodeList } from "../src/print";

test("printNode: prints type", () => {
  const type = f.createTypeReferenceNode("Promise", [
    f.createTypeReferenceNode("void"),
  ]);
  assert.strictEqual(printNode(type), "Promise<void>");
});

test("printNode: prints interface", () => {
  const property = f.createPropertySignature(
    [f.createToken(ts.SyntaxKind.ReadonlyKeyword)],
    "thing",
    f.createToken(ts.SyntaxKind.QuestionToken),
    f.createTypeReferenceNode("T")
  );

  const typeParam = f.createTypeParameterDeclaration(
    /* modifiers */ undefined,
    "T",
    f.createTypeReferenceNode("string")
  );
  const declaration = f.createInterfaceDeclaration(
    /* decorators */ undefined,
    [f.createToken(ts.SyntaxKind.ExportKeyword)],
    "Test",
    [typeParam],
    /* heritageClauses */ undefined,
    [property]
  );

  assert.strictEqual(
    printNode(declaration),
    `export interface Test<T extends string> {
    readonly thing?: T;
}`
  );
});

test("printNodeList: prints statements", () => {
  const interfaceDeclaration = f.createInterfaceDeclaration(
    /* decorators */ undefined,
    /* modifiers */ undefined,
    "Interface",
    /* typeParams */ undefined,
    /* heritageClauses */ undefined,
    []
  );
  const classDeclaration = f.createClassDeclaration(
    /* decorators */ undefined,
    /* modifiers */ undefined,
    "Class",
    /* typeParams */ undefined,
    /* heritageClauses */ undefined,
    []
  );
  const printed = printNodeList([interfaceDeclaration, classDeclaration]);
  assert.strictEqual(
    printed,
    `interface Interface {
}
class Class {
}
`
  );
});
