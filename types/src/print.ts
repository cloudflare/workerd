import ts, { factory as f } from "typescript";

const placeholderFile = ts.createSourceFile(
  "placeholder.ts", // File name doesn't matter here
  "",
  ts.ScriptTarget.ESNext,
  false,
  ts.ScriptKind.TS
);
export const printer = ts.createPrinter({ newLine: ts.NewLineKind.LineFeed });

export function printNode(node: ts.Node): string {
  return printer.printNode(ts.EmitHint.Unspecified, node, placeholderFile);
}

export function printNodeList(nodes: ts.Node[]): string {
  return printer.printList(
    ts.ListFormat.MultiLine,
    f.createNodeArray(nodes),
    placeholderFile
  );
}
