// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { test } from 'node:test';
import { StructureGroups } from '@workerd/jsg/rtti';
import { Message } from 'capnp-es';
import { printDefinitions } from '../src';
import { collectPerIsolateDeclarations } from '../src/per-isolate';

const kManifest = '/per_isolate/public-types.d.ts';

function collect(files: Record<string, string>): string {
  const sources = new Map<string, string>();
  for (const [name, source] of Object.entries(files)) {
    sources.set(`/per_isolate/${name}`, source);
  }
  return collectPerIsolateDeclarations(kManifest, (fileName) =>
    sources.get(fileName)
  );
}

// Declaration files as TypeScript emits them for the bootstrap's modules.
const kThing = `import type { Kind } from '../kind';
/** The thing's options. */
export interface ThingOptions {
    mode?: 'fast' | "slow";
}
interface Thing extends Error {
    /** Iterates the thing. */
    [Symbol.iterator](): IterableIterator<string>;
    readonly size: number;
}
declare class Thing {
    #private;
    constructor(options?: ThingOptions);
    get kind(): Kind;
    static readonly VERSION: number;
    static from(other: Thing): Thing;
}
type Internal = {
    secret: number;
};
declare const helpers: {
    peek(thing: Thing): Internal;
};
export type { Thing };
export type ThingExports = {
    Thing: typeof Thing;
    helpers: typeof helpers;
};
`;
const kKind = `export type Kind = 'a' | 'b';
`;

test('collectPerIsolateDeclarations: prints published declarations in order', () => {
  const printed = collect({
    'public-types.d.ts': `// A comment.
export type { Kind } from './kind';
export type { Thing, ThingOptions } from './thing/thing';
`,
    'thing/thing.d.ts': kThing,
    'kind.d.ts': kKind,
  });
  assert.strictEqual(
    printed,
    `type Kind = 'a' | 'b';
declare class Thing extends Error {
    constructor(options?: ThingOptions);
    get kind(): Kind;
    static readonly VERSION: number;
    static from(other: Thing): Thing;
    /** Iterates the thing. */
    [Symbol.iterator](): IterableIterator<string>;
    readonly size: number;
}
/** The thing's options. */
interface ThingOptions {
    mode?: 'fast' | "slow";
}`
  );
});

test('collectPerIsolateDeclarations: rejects references to unpublished declarations', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Thing, ThingOptions } from './thing/thing';\n`,
        'thing/thing.d.ts': kThing,
        'kind.d.ts': kKind,
      }),
    {
      message:
        'Thing refers to Kind, which /per_isolate/kind.d.ts declares but the bootstrap does not publish',
    }
  );
});

test('collectPerIsolateDeclarations: rejects references through an import that does not resolve', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Thing, ThingOptions } from './thing/thing';\n`,
        'thing/thing.d.ts': kThing,
      }),
    {
      message:
        'Thing refers to Kind, whose import does not resolve to a declaration file',
    }
  );
});

test('collectPerIsolateDeclarations: rejects a manifest that does more than re-export types', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export { Kind } from './kind';\n`,
        'kind.d.ts': kKind,
      }),
    /expected only `export type \{ \.\.\. \} from '\.\.\.'`/
  );
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Sort } from './kind';\n`,
        'kind.d.ts': kKind,
      }),
    { message: `${kManifest}: Sort does not resolve to a declaration` }
  );
});

test('collectPerIsolateDeclarations: rejects a renamed declaration', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Sort } from './kind';\n`,
        'kind.d.ts': `type Kind = 'a';\nexport type { Kind as Sort };\n`,
      }),
    {
      message: `${kManifest}: Sort is Kind renamed; publish a declaration under its own name`,
    }
  );
});

test('collectPerIsolateDeclarations: rejects a base for a class that has one', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Thing } from './thing';\n`,
        'thing.d.ts': `interface Thing extends Error {}
declare class Thing extends EventTarget {}
export type { Thing };
`,
      }),
    {
      message:
        'Thing: a merged interface may give its class a base only if the class has none, and only one',
    }
  );
});

function rootWith(structureName?: string): StructureGroups {
  const root = new Message().initRoot(StructureGroups);
  if (structureName !== undefined) {
    const group = root._initGroups(1).get(0);
    group.name = 'definitions';
    const structure = group._initStructures(1).get(0);
    structure.name = structureName;
    structure.fullyQualifiedName = `workerd::api::${structureName}`;
    structure.tsRoot = true;
  }
  return root;
}

test('printDefinitions: places per-isolate declarations before the defines', () => {
  const { ambient } = printDefinitions(
    rootWith(),
    { Thing: { $: '* A thing. ', size: '* Its size. ' } },
    'declare const defined: Thing;\n',
    'declare class Thing {\n    get size(): number;\n}\n'
  );
  assert.ok(
    ambient.endsWith(`/** A thing. */
declare class Thing {
    /** Its size. */
    get size(): number;
}
declare const defined: Thing;
`),
    ambient
  );
});

test('printDefinitions: rejects a per-isolate name that RTTI or a define declares', () => {
  assert.throws(
    () =>
      printDefinitions(rootWith('Thing'), {}, '', 'declare class Thing {}\n'),
    /Thing is declared by both the per-isolate bootstrap and JSG RTTI/
  );
  assert.throws(
    () =>
      printDefinitions(
        rootWith(),
        {},
        'interface Thing {}\n',
        'declare class Thing {}\n'
      ),
    /Thing is declared by both the per-isolate bootstrap and types\/defines/
  );
});
