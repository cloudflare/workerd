import fs from "node:fs";

let paramNameData: Map<string, Parameter> = new Map();

interface Parameter {
  fullyQualifiedParentName: string;
  methodName: string;
  parameterIndex: number;
  name: string;
}

export function getParameterName(
  fullyQualifiedParentName: string,
  methodName: string,
  parameterIndex: number
): string {
  console.error(`${fullyQualifiedParentName}.${methodName}[${parameterIndex}]`);

  const name = paramNameData.get(
    `${fullyQualifiedParentName}.${methodName}[${parameterIndex}]`
  )?.name;
  // Some parameter names are reserved TypeScript words, which break later down the pipeline
  if (name == "function" || name == "number" || name == "string") {
    return `$${name}`;
  }
  if (name && name != "" && name != "undefined") {
    return name;
  }
  return `param${parameterIndex}`;
}

export function parseApiAstDump(astDumpPath: string) {
  paramNameData = new Map(
    JSON.parse(fs.readFileSync(astDumpPath, { encoding: "utf-8" }))
      .map((p: any) => ({
        fullyQualifiedParentName: p.fully_qualified_parent_name
          .filter((n: any) => !!n)
          .join("::"),
        methodName: p.function_like,
        parameterIndex: p.index,
        name: p.name,
      }))
      .map((p: Parameter) => [
        `${p.fullyQualifiedParentName}.${p.methodName}[${p.parameterIndex}]`,
        p,
      ]) as [string, Parameter][]
  );
}
