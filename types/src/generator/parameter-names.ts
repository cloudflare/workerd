// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import fs from "node:fs";

let paramNameData: Map<string, Parameter> = new Map();

interface Parameter {
  fullyQualifiedParentName: string;
  methodName: string;
  parameterIndex: number;
  name: string;
}

// Support methods defined in parent classes
const additionalClassNames: Record<string, string[]> = {
  DurableObjectStorageOperations: [
    "DurableObjectStorage",
    "DurableObjectTransaction",
  ],
};
export function getParameterName(
  fullyQualifiedParentName: string,
  methodName: string,
  parameterIndex: number
): string {
  const path = `${fullyQualifiedParentName}.${methodName}[${parameterIndex}]`;

  const name = paramNameData.get(path)?.name;
  // Some parameter names are reserved TypeScript words, which break later down the pipeline
  if (name === "function" || name === "number" || name === "string") {
    return `$${name}`;
  }
  if (name && name !== "undefined") {
    return name;
  }
  return `param${parameterIndex}`;
}

export function parseApiAstDump(astDumpPath: string) {
  paramNameData = new Map(
    JSON.parse(fs.readFileSync(astDumpPath, { encoding: "utf-8" }))
      .flatMap((p: any) => {
        const shouldRename = p.fully_qualified_parent_name.find(
          (n: string) => !!additionalClassNames[n]
        );
        const renameOptions = additionalClassNames[shouldRename] ?? [];
        const methodName = p.function_like_name.endsWith("_")
          ? p.function_like_name.slice(0, -1)
          : p.function_like_name;
        return [
          ...renameOptions.map((r) => ({
            fullyQualifiedParentName: p.fully_qualified_parent_name
              .filter((n: any) => !!n)
              .map((n: string) => (n === shouldRename ? r : n))
              .join("::"),
            methodName,
            parameterIndex: p.index,
            name: p.name,
          })),
          {
            fullyQualifiedParentName: p.fully_qualified_parent_name
              .filter((n: any) => !!n)
              .join("::"),
            methodName,
            parameterIndex: p.index,
            name: p.name,
          },
        ];
      })
      .map((p: Parameter) => [
        `${p.fullyQualifiedParentName}.${p.methodName}[${p.parameterIndex}]`,
        p,
      ]) as [string, Parameter][]
  );
}
