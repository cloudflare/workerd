// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export type ParameterNamesData = Record<
  /* fullyQualifiedParentName */ string,
  Record</* functionName */ string, string[] | undefined> | undefined
>;

let data: ParameterNamesData | undefined;
export function installParameterNames(newData: ParameterNamesData) {
  data = newData;
}

const reservedKeywords = ["function", "number", "string"];

export function getParameterName(
  fullyQualifiedParentName: string,
  functionName: string,
  index: number
): string {
  // `constructor` is a reserved property name
  if (functionName === "constructor") functionName = `$${functionName}`;
  const name = data?.[fullyQualifiedParentName]?.[functionName]?.[index];
  if (name === undefined) return `param${index}`;
  if (reservedKeywords.includes(name)) return `$${name}`;
  return name;
}
