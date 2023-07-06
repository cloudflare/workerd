// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
