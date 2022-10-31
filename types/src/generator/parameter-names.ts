import fs from "node:fs";
import path from "node:path";
import process from "node:process";

const HEADERS_CACHE: Record<string, string[]> = {};

function readHeadersInDirectory(directory: string) {
  if (HEADERS_CACHE[directory]) {
    return HEADERS_CACHE[directory];
  }

  const headers = fs
    .readdirSync(directory)
    .filter((filePath) => filePath.endsWith(".h"))
    .map((filePath) =>
      fs.readFileSync(path.join(directory, filePath), { encoding: "utf-8" })
    );

  HEADERS_CACHE[directory] = headers;

  return headers;
}

export function getParameterName(
  fullyQualifiedParentName: string,
  methodName: string,
  parameterIndex: number
): string {
  // const pathToParent = fullyQualifiedParentName.split("::");
  // const parentName = pathToParent.pop();
  // if (!parentName) {
  //   throw new Error(`Unqualified parent: ${fullyQualifiedParentName}`);
  // }

  // const headerDir = path.join(process.cwd(), "src", ...pathToParent);
  // const headers = readHeadersInDirectory(headerDir);

  // for (const header of headers) {
  //   const parentDefinition = header.indexOf(parentName);
  //   if (parentDefinition === -1) {
  //     continue;
  //   }

  //   const methodDefinition = header.indexOf(methodName, parentDefinition);
  //   if (methodDefinition === -1) {
  //     continue;
  //   }

  //   let cursor = header.indexOf("(", methodDefinition);
  //   for (let i = 0; i < parameterIndex; i++) {
  //     cursor = header.indexOf(",")
  //   }
  //   const paramsEnd = header.indexOf(")", paramsStart);

  //   const params = header.substring(paramsStart, paramsEnd);
  //   throw new Error(params);
  // }

  // throw new Error(
  //   `in ${headerDir}: ${parentName}.${methodName} (${parameterIndex})`
  // );

  return `param${parameterIndex}`;
}
