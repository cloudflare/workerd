// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Build-time tool that patches Pyodide's Emscripten-generated `pyodide.asm.js` / `pyodide.asm.mjs`
// so that it can run inside workerd. The file is parsed into an ESTree AST with acorn, the patches
// below are applied as AST rewrites, and the result is printed back to JavaScript with astring.
//
// Each patch declares how many times it must match in each Pyodide version. A mismatch fails the
// build, so a Pyodide upgrade that moves or removes a patch site is caught immediately instead of
// silently shipping an unpatched runtime.
//
// TODO: all of these should be fixed by linking our own Pyodide or by upstreaming.
//
// Usage: patch_pyodide_asm --version <pyodide version> --input <path> --output <path>

import { readFileSync, writeFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import {
  parse,
  parseExpressionAt,
  type AnyNode,
  type CallExpression,
  type Declaration,
  type ExportNamedDeclaration,
  type Expression,
  type FunctionDeclaration,
  type Identifier,
  type ModuleDeclaration,
  type NewExpression,
  type Node,
  type Options,
  type Statement,
  type VariableDeclaration,
} from 'acorn';
import { generate } from 'astring';
import type { Node as EstreeNode } from 'estree';

const ACORN_OPTIONS: Options = { ecmaVersion: 'latest', sourceType: 'module' };
// Replacement snippets are parsed out of context, so relax the checks that depend on it.
const SNIPPET_OPTIONS: Options = {
  ...ACORN_OPTIONS,
  allowReturnOutsideFunction: true,
};

// Versions whose pyodide.asm.js is a CommonJS/UMD-style script that we convert into an ES module.
// Later versions ship pyodide.asm.mjs which is already an ES module.
const COMMONJS_VERSIONS = ['0.26.0a2', '0.28.2'];

// Imports of the wrappers that the patches refer to; prepended to the patched module.
function prelude(imports: string[]): string {
  return `
  import {
    ${imports.join(',\n') + ','}
  } from "pyodide-internal:pool/builtin_wrappers";
  `;
}

// Imports needed to shadow global references in Emscripten's output.
const BASE_IMPORTS = ['addEventListener', 'location'];

// Direct eval is disallowed in esbuild, see: https://esbuild.github.io/content-types/#direct-eval
const EVAL_REPLACEMENT = `(() => {
  throw new Error(
    "Internal Emscripten code tried to eval, this should not happen, please file a bug report"
  );
})()`;

// ---------------------------------------------------------------------------------------------
// AST helpers

type TopLevelStatement = Statement | ModuleDeclaration;

/**
 * What holds the node being visited: the parent node when the node sits in a single-node field,
 * the statement/element list when it sits in an array, or null for the root.
 */
type Container = AnyNode | AnyNode[] | null;

/** Parse a snippet into a single expression node. */
function expr(code: string): Expression {
  return parseExpressionAt(code, 0, SNIPPET_OPTIONS);
}

/** Parse a snippet into a list of statement nodes, which may include import/export declarations. */
function stmts(code: string): TopLevelStatement[] {
  return parse(code, SNIPPET_OPTIONS).body;
}

/** Parse a snippet into exactly one plain (non-module-declaration) statement node. */
function stmt(code: string): Statement {
  const body = stmts(code);
  const [only] = body;
  if (body.length !== 1 || only === undefined) {
    throw new Error(`expected exactly one statement in snippet: ${code}`);
  }
  if (
    only.type === 'ImportDeclaration' ||
    only.type === 'ExportNamedDeclaration' ||
    only.type === 'ExportDefaultDeclaration' ||
    only.type === 'ExportAllDeclaration'
  ) {
    throw new Error(`expected a plain statement, got a ${only.type}: ${code}`);
  }
  return only;
}

/**
 * Build a node that has no position in the source. astring only reads positions when emitting a
 * source map, which we don't.
 */
function synthetic<T extends Node>(fields: Omit<T, 'start' | 'end'>): T {
  return { start: 0, end: 0, ...fields } as T;
}

function identifier(name: string): Identifier {
  return synthetic<Identifier>({ type: 'Identifier', name });
}

function isNode(value: unknown): value is AnyNode {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { type?: unknown }).type === 'string'
  );
}

function isIdentifier(
  node: AnyNode | null | undefined,
  name: string
): node is Identifier {
  return node?.type === 'Identifier' && node.name === name;
}

/**
 * Returns the dotted path of a non-computed member chain rooted at an identifier, e.g.
 * "Function.prototype.apply.apply", or undefined if the node is not such a chain.
 */
function memberPath(node: AnyNode): string | undefined {
  if (node.type === 'Identifier') {
    return node.name;
  }
  if (
    node.type === 'MemberExpression' &&
    !node.computed &&
    node.property.type === 'Identifier'
  ) {
    const objectPath = memberPath(node.object);
    if (objectPath !== undefined) {
      return `${objectPath}.${node.property.name}`;
    }
  }
  return undefined;
}

function isCallOf(node: AnyNode, calleePath: string): node is CallExpression {
  return (
    node.type === 'CallExpression' && memberPath(node.callee) === calleePath
  );
}

function isNewOf(node: AnyNode, calleePath: string): node is NewExpression {
  return (
    node.type === 'NewExpression' && memberPath(node.callee) === calleePath
  );
}

function isFunctionDeclarationNamed(
  node: AnyNode,
  name: string
): node is FunctionDeclaration {
  return node.type === 'FunctionDeclaration' && isIdentifier(node.id, name);
}

/** True for `var/let/const <name> ...` declaring exactly one identifier. */
function isVariableDeclarationOf(
  node: AnyNode,
  name: string
): node is VariableDeclaration {
  return (
    node.type === 'VariableDeclaration' &&
    node.declarations.length === 1 &&
    isIdentifier(node.declarations[0]?.id, name)
  );
}

/** Build `callee(...args)` from a callee snippet and argument nodes. */
function call(
  calleeCode: string,
  args: CallExpression['arguments']
): CallExpression {
  return synthetic<CallExpression>({
    type: 'CallExpression',
    callee: expr(calleeCode),
    arguments: args,
    optional: false,
  });
}

function renameFunction(
  node: FunctionDeclaration,
  name: string
): FunctionDeclaration {
  return { ...node, id: identifier(name) };
}

/** Build `export <declaration>` or, without a declaration, `export { ...names };`. */
function exportNamed({
  declaration = null,
  names = [],
}: {
  declaration?: Declaration | null;
  names?: string[];
}): ExportNamedDeclaration {
  return synthetic<ExportNamedDeclaration>({
    type: 'ExportNamedDeclaration',
    declaration,
    specifiers: names.map((name) =>
      synthetic({
        type: 'ExportSpecifier',
        local: identifier(name),
        exported: identifier(name),
      })
    ),
    source: null,
    attributes: [],
  });
}

// ---------------------------------------------------------------------------------------------
// Patches
//
// The tree is visited post-order, so replacements are never re-visited and patches never see each
// other's output.

/** Number of required matches, either for every version or per version with a `default`. */
type ExpectedCount = number | Record<string, number>;

/** A replacement node, or, when the matched node lives in a statement list, an array of statements that replaces it (empty deletes it). */
type Replacement = AnyNode | AnyNode[];

interface Patch {
  name: string;
  expected: ExpectedCount;
  /** `builtin_wrappers` exports that the replacement refers to; imported when the patch is applied. */
  imports?: string[];
  /** If set, only nodes inside a function declaration with this name are candidates. */
  functionName?: string;
  /** Whether this node is a patch site. `parent` is what contains the node (see Container). */
  match: (node: AnyNode, parent: Container) => boolean;
  replace: (node: AnyNode) => Replacement;
}

/**
 * Define a patch whose `match` is a type predicate, so that `replace` receives the narrowed node
 * type. `replace` is only ever called with nodes that `match` accepted.
 */
function patch<T extends AnyNode>(spec: {
  name: string;
  expected: ExpectedCount;
  imports?: string[];
  functionName?: string;
  match: (node: AnyNode, parent: Container) => node is T;
  replace: (node: T) => Replacement;
}): Patch {
  return { ...spec, replace: (node) => spec.replace(node as T) };
}

const COMMON_PATCHES: Patch[] = [
  patch({
    name: 'new WebAssembly.Module(...) -> newWasmModule(...)',
    expected: { '0.26.0a2': 6, default: 4 },
    imports: ['newWasmModule'],
    match: (node): node is NewExpression => isNewOf(node, 'WebAssembly.Module'),
    replace: (node) => call('newWasmModule', node.arguments),
  }),
  {
    name: 'WebAssembly.instantiate -> wasmInstantiate',
    expected: 2,
    imports: ['wasmInstantiate'],
    match: (node) => memberPath(node) === 'WebAssembly.instantiate',
    replace: () => expr('wasmInstantiate'),
  },
  {
    name: 'Date.now -> monotonicDateNow',
    expected: { '0.26.0a2': 18, default: 22 },
    imports: ['monotonicDateNow'],
    match: (node) => memberPath(node) === 'Date.now',
    replace: () => expr('monotonicDateNow'),
  },
  patch({
    name: 'crypto.getRandomValues(...) -> getRandomValues(Module, ...)',
    expected: 1,
    imports: ['getRandomValues'],
    match: (node): node is CallExpression =>
      isCallOf(node, 'crypto.getRandomValues'),
    replace: (node) =>
      call('getRandomValues', [expr('Module'), ...node.arguments]),
  }),
  {
    name: 'direct eval(...) -> throw',
    expected: 6,
    match: (node) => isCallOf(node, 'eval'),
    replace: () => expr(EVAL_REPLACEMENT),
  },
  // Dynamic linking patches.
  patch({
    // Library lookup: route dynamic library loading through our own loader. `libName` and `flags`
    // are locals of the enclosing `loadDynamicLibrary`, as in the original.
    name: 'function loadLibData() -> patchedLoadLibData',
    expected: 1,
    imports: ['patchedLoadLibData'],
    match: (node): node is FunctionDeclaration =>
      isFunctionDeclarationNamed(node, 'loadLibData'),
    replace: () => [
      stmt(`
        function loadLibData() {
          var libData = patchedLoadLibData(Module, libName, flags.rpath);
          return flags.loadAsync ? Promise.resolve(libData) : libData;
        }
      `),
    ],
  }),
  patch({
    // Ensure the memory base of a dynlib is stable when restoring snapshots.
    name: 'getMemory(...) -> Module.getMemoryPatched(Module, libName, ...)',
    expected: 1,
    match: (node): node is CallExpression => isCallOf(node, 'getMemory'),
    replace: (node) =>
      call('Module.getMemoryPatched', [
        expr('Module'),
        expr('libName'),
        ...node.arguments,
      ]),
  }),
  {
    name: 'log loadWebAssemblyModule after `var tableBase = ...`',
    expected: 1,
    functionName: 'loadModule',
    match: (node, parent) =>
      Array.isArray(parent) && isVariableDeclarationOf(node, 'tableBase'),
    replace: (node) => [
      node,
      stmt(
        "Module.snapshotDebug && console.log('loadWebAssemblyModule', libName, memoryBase, tableBase);"
      ),
    ],
  },
  // Let the runtime observe libffi closures: the table slot handed out to each closure, its release,
  // and the assignment of the trampoline into the slot.
  {
    name: 'record table slot allocations in ffi_closure_alloc_js',
    expected: 1,
    functionName: 'ffi_closure_alloc_js',
    match: (node, parent) =>
      Array.isArray(parent) && isVariableDeclarationOf(node, 'index'),
    replace: (node) => [
      node,
      stmt('Module.recordFfiClosureAlloc?.(closure, index);'),
    ],
  },
  {
    name: 'record table slot frees in ffi_closure_free_js',
    expected: 1,
    functionName: 'ffi_closure_free_js',
    match: (node, parent) =>
      Array.isArray(parent) && isVariableDeclarationOf(node, 'index'),
    replace: (node) => [
      node,
      stmt('Module.recordFfiClosureFree?.(closure, index);'),
    ],
  },
  {
    name: 'record table slot assignments in ffi_prep_closure_loc_js',
    expected: 1,
    functionName: 'ffi_prep_closure_loc_js',
    match: (node, parent) =>
      Array.isArray(parent) &&
      node.type === 'ExpressionStatement' &&
      isCallOf(node.expression, 'setWasmTableEntry'),
    replace: (node) => [
      node,
      stmt('Module.recordFfiPrepClosureLoc?.(closure, codeloc, sig);'),
    ],
  },
];

const PATCHES_0_26_0A2: Patch[] = [
  {
    name: 'Remove reportUndefinedSymbols()',
    expected: 3,
    match: (node, parent) =>
      Array.isArray(parent) &&
      node.type === 'ExpressionStatement' &&
      isCallOf(node.expression, 'reportUndefinedSymbols') &&
      node.expression.arguments.length === 0,
    replace: () => [],
  },
  patch({
    name: 'function _PyEM_CountFuncParams(func) -> patched_PyEM_CountFuncParams',
    expected: 1,
    imports: ['patched_PyEM_CountFuncParams'],
    match: (node): node is FunctionDeclaration =>
      isFunctionDeclarationNamed(node, '_PyEM_CountFuncParams'),
    replace: (node) => ({
      ...node,
      body: {
        ...node.body,
        body: [stmt('return patched_PyEM_CountFuncParams(Module, func);')],
      },
    }),
  }),
  patch({
    // To fix RPC, applies https://github.com/pyodide/pyodide/commit/8da1f38f7.
    name: 'nullToUndefined(func.apply(...)) -> nullToUndefined(patchedApplyFunc(API, func, ...))',
    expected: 2,
    imports: ['patchedApplyFunc'],
    match: (node): node is CallExpression & { arguments: [CallExpression] } =>
      isCallOf(node, 'nullToUndefined') &&
      node.arguments.length === 1 &&
      node.arguments[0] !== undefined &&
      isCallOf(node.arguments[0], 'func.apply'),
    replace: (node) =>
      call('nullToUndefined', [
        call('patchedApplyFunc', [
          expr('API'),
          expr('func'),
          ...node.arguments[0].arguments,
        ]),
      ]),
  }),
];

// Hot fix for https://github.com/pyodide/pyodide/pull/6466
const STACK_SWITCH_GC_HOTFIX_314_0_6: Patch[] = [
  patch({
    name: 'clear and record PyThreadState.current_frame',
    expected: 1,
    match: (node, parent): node is FunctionDeclaration =>
      Array.isArray(parent) && isFunctionDeclarationNamed(node, 'saveState'),
    replace: (node) => [
      stmt(`
        function saveState() {
          const state = origSaveState();
          if (state === Module.error) {
            return state;
          }
          state.currentFrame = HEAP32[(state.threadState + 52) / 4];
          HEAP32[(state.threadState + 52) / 4] = 0;
          return state;
        }
      `),
      renameFunction(node, 'origSaveState'),
    ],
  }),
  patch({
    name: 'restore PyThreadState.current_frame',
    expected: 1,
    match: (node, parent): node is FunctionDeclaration =>
      Array.isArray(parent) && isFunctionDeclarationNamed(node, 'restoreState'),
    replace: (node) => [
      stmt(`
        function restoreState(state) {
          origRestoreState(state);
          HEAP32[(state.threadState + 52) / 4] = state.currentFrame;
        }
      `),
      renameFunction(node, 'origRestoreState'),
    ],
  }),
];

// pyodide.asm.js in these versions is a CommonJS/UMD-style script; convert it to an ES module.
// When we link our own Pyodide we can pass `-sES6_MODULE` to the linker and it will do this for us
// automatically.
const COMMONJS_PATCHES: Patch[] = [
  patch({
    name: 'var _createPyodideModule -> export const _createPyodideModule',
    expected: 1,
    match: (node, parent): node is VariableDeclaration =>
      Array.isArray(parent) &&
      isVariableDeclarationOf(node, '_createPyodideModule'),
    replace: (node) => exportNamed({ declaration: { ...node, kind: 'const' } }),
  }),
  {
    name: 'remove `globalThis._createPyodideModule = _createPyodideModule;`',
    expected: 1,
    match: (node, parent) =>
      Array.isArray(parent) &&
      node.type === 'ExpressionStatement' &&
      node.expression.type === 'AssignmentExpression' &&
      node.expression.operator === '=' &&
      memberPath(node.expression.left) === 'globalThis._createPyodideModule' &&
      isIdentifier(node.expression.right, '_createPyodideModule'),
    replace: () => [],
  },
];

// pyodide.asm.mjs in later versions is already an ES module.
const ES_MODULE_PATCHES: Patch[] = [
  {
    name: 'export default _createPyodideModule -> default and named export',
    expected: 1,
    match: (node, parent) =>
      Array.isArray(parent) &&
      node.type === 'ExportDefaultDeclaration' &&
      isIdentifier(node.declaration, '_createPyodideModule'),
    replace: (node) => [
      node,
      // Still expose _createPyodideModule for compatibility (import { _createPyodideModule }).
      exportNamed({ names: ['_createPyodideModule'] }),
    ],
  },
];

function patchesForVersion(version: string): Patch[] {
  const patches = [...COMMON_PATCHES];
  if (version === '0.26.0a2') {
    patches.push(...PATCHES_0_26_0A2);
  } else if (version === '314.0.6') {
    patches.push(...STACK_SWITCH_GC_HOTFIX_314_0_6);
  }
  patches.push(
    ...(COMMONJS_VERSIONS.includes(version)
      ? COMMONJS_PATCHES
      : ES_MODULE_PATCHES)
  );
  return patches;
}

function expectedCount(spec: Patch, version: string): number {
  if (typeof spec.expected === 'number') {
    return spec.expected;
  }
  const count = spec.expected[version] ?? spec.expected['default'];
  if (count === undefined) {
    throw new Error(
      `patch "${spec.name}" has no expected count for version ${version}`
    );
  }
  return count;
}

// ---------------------------------------------------------------------------------------------
// Tree walking

/**
 * Post-order traversal applying the patches. `container[key]` is the child being visited: either a
 * node in an object field, or a node at an index in a statement/element array. Returns how much
 * the containing array grew (or shrank) if the node was replaced by a statement list, so that the
 * caller can resume iteration after the replacement.
 */
function visit(
  node: AnyNode,
  container: Container,
  key: string | number,
  patches: Patch[],
  counts: Map<string, number>,
  enclosingFunctions: string[]
): number {
  // Names of the function declarations the children are nested in, innermost last.
  const childScope =
    node.type === 'FunctionDeclaration' && node.id !== null
      ? [...enclosingFunctions, node.id.name]
      : enclosingFunctions;

  // Visit children first so that replacements are never re-visited.
  const fields = node as unknown as Record<string, unknown>;
  for (const childKey of Object.keys(fields)) {
    const child = fields[childKey];
    if (Array.isArray(child)) {
      // Iterate by index so that splices performed by replacements are accounted for.
      const list = child as unknown[];
      for (let i = 0; i < list.length; i++) {
        const element = list[i];
        if (isNode(element)) {
          i += visit(
            element,
            list as AnyNode[],
            i,
            patches,
            counts,
            childScope
          );
        }
      }
    } else if (isNode(child)) {
      visit(child, node, childKey, patches, counts, childScope);
    }
  }

  for (const spec of patches) {
    if (
      spec.functionName !== undefined &&
      !enclosingFunctions.includes(spec.functionName)
    ) {
      continue;
    }
    if (!spec.match(node, container)) {
      continue;
    }
    counts.set(spec.name, (counts.get(spec.name) ?? 0) + 1);
    const replacement = spec.replace(node);
    if (Array.isArray(replacement)) {
      if (!Array.isArray(container) || typeof key !== 'number') {
        throw new Error(
          `patch "${spec.name}" produced a statement list but matched a node that is not in a list`
        );
      }
      container.splice(key, 1, ...replacement);
      // A subsequent patch cannot match this node anymore, so return immediately.
      return replacement.length - 1;
    }
    if (container === null) {
      throw new Error(`patch "${spec.name}" tried to replace the root node`);
    }
    if (Array.isArray(container)) {
      container[key as number] = replacement;
    } else {
      (container as unknown as Record<string, unknown>)[key] = replacement;
    }
    return 0;
  }
  return 0;
}

function patchSource(source: string, version: string): string {
  const patches = patchesForVersion(version);
  const ast = parse(source, ACORN_OPTIONS);
  const counts = new Map<string, number>();
  visit(ast, null, 0, patches, counts, []);
  // Import the wrappers the applied patches refer to. Done after visiting so that the prelude
  // itself is never a candidate for patching.
  const imports = new Set(BASE_IMPORTS);
  for (const spec of patches) {
    if ((counts.get(spec.name) ?? 0) > 0) {
      for (const name of spec.imports ?? []) {
        imports.add(name);
      }
    }
  }
  ast.body.unshift(...stmts(prelude([...imports].sort())));

  const mismatches: string[] = [];
  for (const spec of patches) {
    const actual = counts.get(spec.name) ?? 0;
    const expected = expectedCount(spec, version);
    if (actual !== expected) {
      mismatches.push(
        `  ${spec.name}: expected ${expected} match(es), got ${actual}`
      );
    }
  }
  if (mismatches.length > 0) {
    throw new Error(
      `Pyodide ${version}: patch site count mismatch. Either Pyodide changed, or the expected ` +
        `counts in patch_pyodide_asm.ts need to be updated:\n${mismatches.join('\n')}`
    );
  }

  // acorn's node types are a superset of ESTree's (they carry `start`/`end`), but the two type
  // libraries model literals differently, so the compiler does not consider them assignable.
  return generate(ast as unknown as EstreeNode);
}

function main(): void {
  const { values } = parseArgs({
    options: {
      version: { type: 'string' },
      input: { type: 'string' },
      output: { type: 'string' },
    },
    strict: true,
  });
  const { version, input, output } = values;
  if (version === undefined || input === undefined || output === undefined) {
    throw new Error(
      'usage: --version <version> --input <path> --output <path>'
    );
  }
  const source = readFileSync(input, 'utf8');
  writeFileSync(output, patchSource(source, version));
}

main();
