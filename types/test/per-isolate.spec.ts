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
      message: `Thing refers to Kind, which /per_isolate/kind.d.ts declares but ${kManifest} does not publish`,
    }
  );
});

test('collectPerIsolateDeclarations: rejects a published declaration imported under another name', () => {
  // Printed as written, the parameter's type would be the global Date.
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Thing } from './thing';
export type { PublicOptions } from './options';
`,
        'options.d.ts': `export interface PublicOptions {\n    mode: string;\n}\n`,
        'thing.d.ts': `import type { PublicOptions as Date } from './options';
declare class Thing {
    constructor(options: Date);
}
export type { Thing };
`,
      }),
    {
      message:
        'Thing refers to PublicOptions as Date; import it under its own name',
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

test('collectPerIsolateDeclarations: checks the references in computed member names', () => {
  // A unique symbol that the module declares is not in the output.
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Thing } from './thing';\n`,
        'thing.d.ts': `declare const kSlots: unique symbol;
declare class Thing {
    [kSlots]: number;
}
export type { Thing };
`,
      }),
    {
      message: `Thing refers to kSlots, which /per_isolate/thing.d.ts declares but ${kManifest} does not publish`,
    }
  );
  // A global's symbol and a literal key are.
  assert.strictEqual(
    collect({
      'public-types.d.ts': `export type { Thing } from './thing';\n`,
      'thing.d.ts': `declare class Thing {
    [Symbol.iterator](): void;
    ['read'](): void;
}
export type { Thing };
`,
    }),
    `declare class Thing {
    [Symbol.iterator](): void;
    ['read'](): void;
}`
  );
});

test('collectPerIsolateDeclarations: rejects an import type', () => {
  // TypeScript writes these for an inferred type that the module does not
  // name, such as a getter's without a return type.
  for (const type of [
    'import("./state").State',
    'typeof import("./state")',
  ]) {
    assert.throws(
      () =>
        collect({
          'public-types.d.ts': `export type { Thing } from './thing';\n`,
          'state.d.ts': `export type State = 'open' | 'closed';\n`,
          'thing.d.ts': `declare class Thing {
    get state(): ${type};
}
export type { Thing };
`,
        }),
      {
        message: `Thing refers to \`${type}\`, which the Workers types cannot resolve; annotate the type with a global or a published declaration`,
      }
    );
  }
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

test('collectPerIsolateDeclarations: rejects a member declared by both a class and a merged interface', () => {
  // TypeScript tries the later interface's overload first, which the folded
  // class would try last. A literal key names the same member.
  for (const key of ['read', "['read']", "'read'"]) {
    assert.throws(
      () =>
        collect({
          'public-types.d.ts': `export type { Thing } from './thing';\n`,
          'thing.d.ts': `declare class Thing {
    read(value: object): 'generic';
}
interface Thing {
    ${key}(value: { id: string }): 'specific';
}
export type { Thing };
`,
        }),
      {
        message:
          'Thing: read is declared by more than one of the class and the interfaces merged with it; declare it, with all its overloads, in one of them',
      }
    );
  }
  // A static member does not merge with an instance member.
  assert.strictEqual(
    collect({
      'public-types.d.ts': `export type { Thing } from './thing';\n`,
      'thing.d.ts': `declare class Thing {
    static read(): Thing;
}
interface Thing {
    read(): string;
}
export type { Thing };
`,
    }),
    `declare class Thing {
    static read(): Thing;
    read(): string;
}`
  );
});

test('collectPerIsolateDeclarations: rejects a private or protected member', () => {
  // TypeScript emits these, unlike #private members.
  for (const [member, memberName] of [
    ['private store;', 'store'],
    ['protected flush(): void;', 'flush'],
  ]) {
    assert.throws(
      () =>
        collect({
          'public-types.d.ts': `export type { Thing } from './thing';\n`,
          'thing.d.ts': `declare class Thing {
    #private;
    ${member}
}
export type { Thing };
`,
        }),
      {
        message: `Thing: ${memberName} is private or protected, which the Workers types would publish; make it a #private member`,
      }
    );
  }
  // A class that users cannot construct.
  assert.strictEqual(
    collect({
      'public-types.d.ts': `export type { Thing } from './thing';\n`,
      'thing.d.ts': `declare class Thing {
    private constructor();
}
export type { Thing };
`,
    }),
    `declare class Thing {
    private constructor();
}`
  );
});

test('collectPerIsolateDeclarations: gives a class the type parameter constraints and defaults of a merged interface', () => {
  const printed = collect({
    'public-types.d.ts': `export type { Thing } from './thing';\n`,
    'thing.d.ts': `declare class Thing<T, U = number> {
    get(): T;
}
interface Thing<T extends string = 'a', U = number> {
    other(): U;
}
export type { Thing };
`,
  });
  assert.strictEqual(
    printed,
    `declare class Thing<T extends string = 'a', U = number> {
    get(): T;
    other(): U;
}`
  );
});

// A hand-written declaration file in the standard library's form.
const kGadget = `// A header that is not part of the declarations.

/** A gadget. */
interface Gadget extends Error {
    readonly code: number; // A trailing note.
    // A note on the constants.
    readonly FIRST: 1;
    /* Another note. */
}
declare var Gadget: {
    prototype: Gadget;
    new (message?: string): Gadget;
    readonly FIRST: 1;
};
export type { Gadget };
`;

test('collectPerIsolateDeclarations: prints an interface with its constructor variable', () => {
  const printed = collect({
    'public-types.d.ts': `export type { Gadget } from './gadget.public';\n`,
    'gadget.public.d.ts': kGadget,
  });
  assert.strictEqual(
    printed,
    `/** A gadget. */
interface Gadget extends Error {
    readonly code: number;
    readonly FIRST: 1;
}
declare var Gadget: {
    prototype: Gadget;
    new (message?: string): Gadget;
    readonly FIRST: 1;
};`
  );
});

test('collectPerIsolateDeclarations: rejects a variable that is not an interface constructor', () => {
  const message =
    'Gadget: a published variable declares the constructor of an interface, which has no other declarations';
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Gadget } from './gadget';\n`,
        'gadget.d.ts': `declare var Gadget: number;\nexport type { Gadget };\n`,
      }),
    { message }
  );
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Gadget } from './gadget';\n`,
        'gadget.d.ts': `type Gadget = string;
declare var Gadget: { new (): Gadget };
export type { Gadget };
`,
      }),
    { message }
  );
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Gadget } from './gadget';\n`,
        'gadget.d.ts': `interface Gadget {}
declare var Gadget: { new (): Gadget }, gadgets: Gadget[];
export type { Gadget };
`,
      }),
    { message: 'Gadget: a published variable is declared on its own' }
  );
});

test('collectPerIsolateDeclarations: rejects an @internal member of a hand-written declaration file', () => {
  assert.throws(
    () =>
      collect({
        'public-types.d.ts': `export type { Gadget } from './gadget.public';\n`,
        'gadget.public.d.ts': `interface Gadget {
    open(): void;
    /** @internal */
    secret(): void;
}
declare var Gadget: {
    prototype: Gadget;
    new (): Gadget;
};
export type { Gadget };
`,
      }),
    {
      message:
        'Gadget: /per_isolate/gadget.public.d.ts marks a declaration @internal, which would be published; leave it out of the file',
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
    /Thing is published by src\/per_isolate\/public-types\.d\.ts and also declared by JSG RTTI/
  );
  assert.throws(
    () =>
      printDefinitions(
        rootWith(),
        {},
        'interface Thing {}\n',
        'declare class Thing {}\n'
      ),
    /Thing is published by src\/per_isolate\/public-types\.d\.ts and also declared in types\/defines/
  );
});
