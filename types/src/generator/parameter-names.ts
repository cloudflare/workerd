import fs from "node:fs";

let paramNameData: any = {};

export function getParameterName(
  fullyQualifiedParentName: string,
  methodName: string,
  parameterIndex: number
): string {
  console.error(`${fullyQualifiedParentName}.${methodName}[${parameterIndex}]`);
  return `param${parameterIndex}`;
}

export function parseApiAstDump(astDumpPath: string) {
  paramNameData = JSON.parse(
    fs.readFileSync(astDumpPath, { encoding: "utf-8" })
  );

  // console.error(JSON.stringify(paramNameData));
}
