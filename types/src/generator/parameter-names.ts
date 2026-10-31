// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export type ParameterNamesData = Record<
  /* fullyQualifiedParentName */ string,
  Record</* functionName */ string, string[] | undefined> | undefined
>;

let data: ParameterNamesData | undefined;
export function installParameterNames(newData: ParameterNamesData): void {
  data = newData;
}

const reservedKeywords = new Set([
  'arguments',
  'await',
  'break',
  'case',
  'catch',
  'class',
  'const',
  'continue',
  'debugger',
  'default',
  'delete',
  'do',
  'else',
  'enum',
  'eval',
  'export',
  'extends',
  'false',
  'finally',
  'for',
  'function',
  'if',
  'implements',
  'import',
  'in',
  'instanceof',
  'interface',
  'let',
  'new',
  'null',
  'number',
  'package',
  'private',
  'protected',
  'public',
  'return',
  'static',
  'string',
  'super',
  'switch',
  'this',
  'throw',
  'true',
  'try',
  'typeof',
  'var',
  'void',
  'while',
  'with',
  'yield',
]);

export function getParameterName(
  fullyQualifiedParentName: string,
  functionName: string,
  index: number,
  explicitName?: string
): string {
  // `constructor` is a reserved property name
  if (functionName === 'constructor') functionName = `$${functionName}`;
  const name =
    explicitName || data?.[fullyQualifiedParentName]?.[functionName]?.[index];
  if (name === undefined) return `param${index}`;
  if (reservedKeywords.has(name)) return `$${name}`;
  return name;
}
