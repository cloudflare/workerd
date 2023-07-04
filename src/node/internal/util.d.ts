// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export abstract class MIMEType {
  public constructor(input: string);
  public type: string;
  public subtype: string;
  public readonly essence: string;
  public readonly params: MIMEParams;
  public toString(): string;
  public toJSON(): string;
}

export abstract class MIMEParams {
  public constructor();
  public delete(name: string): void;
  public get(name: string): string|undefined;
  public has(name: string): boolean;
  public set(name: string, value: string): void;
  public entries(): Iterable<string[]>;
  public keys(): Iterable<string>;
  public values(): Iterable<string>;
}
