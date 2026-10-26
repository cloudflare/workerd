// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { StructureGroups } from '@workerd/jsg/rtti';
import { Message } from 'capnp-es';
import ts from 'typescript';
import { generateDefinitions } from '../../src/generator';
import { printDefinitions } from '../../src';
import { printNodeList } from '../../src/print';

function readFixture(args: string[] = []): StructureGroups {
  const path = process.env.RUST_RTTI_FIXTURE_BINARY;
  assert(path);
  const bytes = execFileSync(path, args);
  return new Message(bytes, false).getRoot(StructureGroups);
}

test('Rust bindings generate TypeScript through the shared RTTI schema', () => {
  const root = readFixture();
  const { nodes, structureMap } = generateDefinitions(root);
  const source = printNodeList(nodes);
  assert.equal(structureMap.size, 8);
  const structures = root.groups.get(0).structures.toArray();
  assert.equal(structures.length, 8);
  assert.equal(
    new Set(structures.map((structure) => structure.fullyQualifiedName)).size,
    8
  );
  assert.deepEqual(
    root.modules.toArray().map((module) => module.specifier),
    ['test:first', 'test:second', 'test:versioned']
  );
  assert.equal(source.match(/class RustRttiFixture/g)?.length, 1);
  assert.match(source, /class RustRttiFixture/);
  assert.match(source, /constructor\(label\?: string\)/);
  assert.match(
    source,
    /lookup\(label: string, detail: string \| null\): RustRttiResult/
  );
  // A Vec of numbers is an Array; a typed array is the explicit `v8` handle type.
  assert.match(source, /copyBytes\(bytes: number\[\]\): number\[\]/);
  assert.match(source, /typedBytes\(bytes: number\[\]\): Uint16Array/);
  assert.match(source, /wide\(value: number \| bigint\): number \| bigint/);
  assert.match(source, /echo\(value: Uint8Array\): Uint8Array/);
  assert.match(source, /labels\(labels: string\[\]\): string\[\]/);
  assert.match(
    source,
    /present\(values: Record<string, number \| undefined>\): Record<string, number>/
  );
  assert.match(source, /static choose\(value: string \| number\): string/);
  assert.match(source, /payloadKind\(payload: Uint8Array \| string\): string/);
  assert.match(source, /get title\(\): string/);
  assert.doesNotMatch(
    source,
    /set title|debugLabel|Unreachable|_lock|ResultData/
  );
  assert.match(source, /get child\(\): RustRttiFixture \| null;/);
  assert.match(
    source,
    /set child\(value: RustRttiFixture \| null\);/
  );
  // `Nullable<T>` is `T | null`, so a field or parameter of that type is required, while
  // `Option<Nullable<T>>` is optional.
  assert.match(source, /\n\s+detail: string \| null;/);
  assert.doesNotMatch(source, /detail\?/);
  assert.match(
    source,
    /hint\(hint\?: string \| null\): string \| null \| undefined/
  );
  // `NullOrUndefined<T>` is `T | null | undefined`, so a parameter or field of that type is
  // optional.
  assert.match(
    source,
    /pick\(value\?: string \| null\): string \| null \| undefined/
  );
  assert.match(source, /\n\s+note\?: string \| null;/);
  assert.match(source, /static readonly HALF: number/);
  assert.match(
    source,
    /reserved\(\$new: string, \$class: string, \$default: string, \$delete: string, \$this: string\): string/
  );
  assert.match(
    source,
    /parameterNames\(recordType: string, unusedArg: string, \$void: string\): string/
  );
  assert.match(source, /makeSymbol\(\): symbol/);
  assert.match(source, /makeName\(\): string \| symbol/);
  assert.match(source, /errorMessage\(error: Error\): string/);
  assert.match(source, /later\(when: number \| Date\): Date/);
  assert.match(source, /bigValue\(\): number \| bigint/);
  assert.match(source, /epoch\(\): Date/);
  // A nested type is declared as a class, even when nesting is the only way to reach it,
  // along with the types that only it refers to.
  assert.match(source, /\n\s+Rows: typeof RustRttiChild;/);
  assert.match(source, /class RustRttiChild \{/);
  // A constructor parameter named `this` is an argument, not a receiver.
  assert.match(source, /constructor\(\$this: string\)/);
  assert.match(source, /interface RustRttiStructRoot/);
  assert.match(source, /detail\(\): RustRttiChildDetail/);
  assert.match(source, /interface RustRttiChildDetail/);
  // Like C++ promise arguments, a value that is not a promise is accepted and resolved.
  assert.match(
    source,
    /settle\(promise: any \| Promise<any>\): Promise<any>/
  );
  assert.match(source, /\n\s+withThis\(suffix: string\): string/);

  // Members are described in registration order, as in C++.
  const fixture = root.groups.get(0).structures.get(0);
  assert(fixture.members.get(0)._isConstructor);
  assert.equal(fixture.members.get(1).method.name, 'lookup');
  const members = fixture.members.toArray();
  // Nested types are registered after the other members, as at runtime.
  assert.deepEqual(
    members.slice(-4).map((member) =>
      member._isConstant
        ? member.constant.name
        : member._isNested
          ? member.nested.name
          : member.property.name
    ),
    ['title', 'child', 'HALF', 'Rows']
  );
  const child = structures.find(
    (structure) => structure.name === 'RustRttiChild'
  );
  assert(child);
  const kind = child.members
    .toArray()
    .find((member) => member._isProperty && member.property.name === 'kind');
  assert(kind?.property.lazy);
  const constant = fixture.members
    .toArray()
    .find((member) => member._isConstant);
  assert(constant);
  assert.equal(constant.constant.numberValue, 1.5);

  const { ambient, importable } = printDefinitions(root, {}, '');
  assert.match(ambient, /declare class RustRttiFixture/);
  assert.match(importable, /export (?:declare )?class RustRttiFixture/);

  const usage = `
    const fixture = new RustRttiFixture();
    const result: RustRttiResult = fixture.lookup("label", null);
    // @ts-expect-error a Nullable argument is required
    fixture.lookup("label");
    fixture.hint();
    fixture.hint(null);
    fixture.pick();
    fixture.pick(null);
    fixture.child = fixture;
    fixture.child = null;
    const bytes: number[] = fixture.copyBytes([1, 2]);
    const typed: Uint16Array = fixture.typedBytes([1, 2]);
    const echoed: Uint8Array = fixture.echo(new Uint8Array());
    const choice: string = RustRttiFixture.choose(42);
    const symbol: symbol = fixture.makeSymbol();
    const name: string | symbol = fixture.makeName();
    const view: ArrayBufferView = fixture.asTypedArray(new Uint8Array());
    const halfWords: ReturnType<RustRttiFixture["asTypedArray"]> = new Float16Array();
    const message: string = fixture.errorMessage(new Error());
    const epoch: Date = fixture.epoch();
    const nested: RustRttiChild = new fixture.Rows("child");
    const structRoot: RustRttiStructRoot = { label: "root" };
    const depth: number = nested.detail().depth;
    fixture.reserved("a", "b", "c", "d", "e");
    fixture.parameterNames("a", "b", "c");
    // @ts-expect-error Option accepts undefined, not null
    new RustRttiFixture(null);
    // @ts-expect-error the property is read-only
    fixture.title = "changed";
    // @ts-expect-error a Vec of numbers is an Array, not a typed array
    fixture.copyBytes(new Uint16Array());
  `;
  const options: ts.CompilerOptions = {
    strict: true,
    noEmit: true,
    target: ts.ScriptTarget.ESNext,
    lib: ['lib.esnext.d.ts'],
  };
  const host = ts.createCompilerHost(options);
  const readSource = host.getSourceFile.bind(host);
  host.getSourceFile = (path, version, onError, shouldCreate) => {
    const source =
      path === '/fixture.d.ts'
        ? ambient
        : path === '/usage.ts'
          ? usage
          : undefined;
    return source === undefined
      ? readSource(path, version, onError, shouldCreate)
      : ts.createSourceFile(path, source, ts.ScriptTarget.ESNext);
  };
  const program = ts.createProgram(
    ['/fixture.d.ts', '/usage.ts'],
    options,
    host
  );
  assert.deepEqual(
    ts
      .getPreEmitDiagnostics(program)
      .map((diagnostic) =>
        ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')
      ),
    []
  );
});

/** The members of `RustRttiVersioned` in the fixture exported with `args`. */
function versionedMembers(args: string[]): string[] {
  const versioned = readFixture(args)
    .groups.get(0)
    .structures.toArray()
    .find((structure) => structure.name === 'RustRttiVersioned');
  assert(versioned);
  return versioned.members.toArray().map((member) => {
    if (member._isMethod) {
      return `method ${member.method.name}`;
    }
    assert(member._isProperty);
    const { name, prototype, readonly } = member.property;
    return `${prototype ? 'prototype' : 'instance'} ${readonly ? 'readonly ' : ''}${name}`;
  });
}

test('Rust RTTI describes the members registered under the export flags', () => {
  assert.deepEqual(versionedMembers([]), ['instance readonly mode']);
  assert.deepEqual(versionedMembers(['--flagged']), [
    'prototype readonly mode',
    'method cached',
  ]);
});

test('Rust definitions are omitted when their exposure root is absent', () => {
  const root = readFixture();
  const structures = root.groups.get(0).structures.toArray();
  const resourceRoot = structures.find(
    (structure) => structure.name === 'RustRttiFixture'
  );
  assert(resourceRoot);
  resourceRoot.tsRoot = false;
  // A struct root is emitted without the resource root.
  const source = printNodeList(generateDefinitions(root).nodes);
  assert.match(source, /interface RustRttiStructRoot/);
  assert.doesNotMatch(source, /RustRttiFixture|RustRttiChild/);
  for (const structure of structures) {
    structure.tsRoot = false;
  }
  assert.equal(generateDefinitions(root).nodes.length, 0);
});
