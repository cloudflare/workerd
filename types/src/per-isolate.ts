// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Collects the declarations that the per-isolate bootstrap (src/per_isolate)
// publishes, from the declaration files TypeScript emits for it. The
// bootstrap's manifest re-exports each published declaration with
// `export type { Name } from './module'`; see "Per-isolate declarations" in
// types/AGENTS.md for how a class declares its public surface there.

import ts, { factory as f } from 'typescript';
import { printer } from './print';

// The bootstrap's manifest, as a path relative to src/per_isolate.
export const PER_ISOLATE_MANIFEST = 'public-types.d.ts';

type PublishedDeclaration = ts.Statement &
  (ts.ClassDeclaration | ts.InterfaceDeclaration | ts.TypeAliasDeclaration);

// A program of the manifest and the declaration files its module specifiers
// reach, and no others: the directory may also hold the modules' sources,
// which would shadow their declarations, and unrelated declaration files.
// `readFile` returns a file's contents, or undefined if there is none.
function createDeclarationProgram(
  manifestPath: string,
  readFile: (fileName: string) => string | undefined
): ts.Program {
  const read = (fileName: string): string | undefined =>
    fileName.endsWith('.d.ts') ? readFile(fileName) : undefined;
  const host: ts.CompilerHost = {
    getSourceFile(fileName, languageVersion) {
      const text = read(fileName);
      return text === undefined
        ? undefined
        : ts.createSourceFile(fileName, text, languageVersion);
    },
    getDefaultLibFileName: () => 'lib.d.ts',
    writeFile() {
      throw new Error('writeFile() not implemented');
    },
    getCurrentDirectory: () => '/',
    getCanonicalFileName: (fileName) => fileName,
    useCaseSensitiveFileNames: () => true,
    getNewLine: () => '\n',
    fileExists: (fileName) => read(fileName) !== undefined,
    readFile: read,
  };
  return ts.createProgram(
    [manifestPath],
    {
      noLib: true,
      types: [],
      module: ts.ModuleKind.ESNext,
      moduleResolution: ts.ModuleResolutionKind.Bundler,
    },
    host
  );
}

// Returns the declarations `manifestPath` publishes, in its order, as
// ambient TypeScript source. `manifestPath` is absolute, and `readFile`
// reads the declaration files its module specifiers resolve to. Throws if
// the manifest publishes something other than a class, interface or type
// alias, or if a published declaration refers to one of the bootstrap's
// declarations that the manifest does not publish.
export function collectPerIsolateDeclarations(
  manifestPath: string,
  readFile: (fileName: string) => string | undefined
): string {
  const program = createDeclarationProgram(manifestPath, readFile);
  const checker = program.getTypeChecker();
  const manifest = program.getSourceFile(manifestPath);
  if (manifest === undefined) {
    throw new Error(`${manifestPath} does not exist`);
  }

  const published = resolveManifest(checker, manifest);
  const publishedSymbols = new Set(published.values());
  const printed: string[] = [];
  for (const [name, symbol] of published) {
    const declarations = publishedDeclarationsOf(name, symbol);
    for (const declaration of declarations) {
      checkReferences(checker, name, declaration, publishedSymbols);
    }
    for (const declaration of mergeDeclarations(name, declarations)) {
      printed.push(
        printer.printNode(
          ts.EmitHint.Unspecified,
          declaration,
          declarations[0].getSourceFile()
        )
      );
    }
  }
  return printed.join('\n');
}

// The manifest's published names, in order, each with the symbol it
// re-exports.
function resolveManifest(
  checker: ts.TypeChecker,
  manifest: ts.SourceFile
): Map<string, ts.Symbol> {
  const published = new Map<string, ts.Symbol>();
  for (const statement of manifest.statements) {
    if (
      !ts.isExportDeclaration(statement) ||
      !statement.isTypeOnly ||
      statement.moduleSpecifier === undefined ||
      statement.exportClause === undefined ||
      !ts.isNamedExports(statement.exportClause)
    ) {
      throw new Error(
        `${manifest.fileName}: expected only \`export type { ... } from '...'\`, found \`${statement.getText(manifest)}\``
      );
    }
    for (const element of statement.exportClause.elements) {
      const name = element.name.text;
      const alias = checker.getSymbolAtLocation(element.name);
      const symbol =
        alias === undefined ? undefined : checker.getAliasedSymbol(alias);
      if (symbol?.declarations === undefined) {
        throw new Error(
          `${manifest.fileName}: ${name} does not resolve to a declaration`
        );
      }
      // Re-exported under another name, the declaration would still print
      // under its own.
      if (symbol.name !== name) {
        throw new Error(
          `${manifest.fileName}: ${name} is ${symbol.name} renamed; publish a declaration under its own name`
        );
      }
      if (published.has(name)) {
        throw new Error(`${manifest.fileName}: ${name} is published twice`);
      }
      published.set(name, symbol);
    }
  }
  return published;
}

function publishedDeclarationsOf(
  name: string,
  symbol: ts.Symbol
): PublishedDeclaration[] {
  const declarations = symbol.declarations ?? [];
  for (const declaration of declarations) {
    if (
      !ts.isClassDeclaration(declaration) &&
      !ts.isInterfaceDeclaration(declaration) &&
      !ts.isTypeAliasDeclaration(declaration)
    ) {
      throw new Error(
        `${name} is a ${ts.SyntaxKind[declaration.kind]}; only classes, interfaces and type aliases can be published`
      );
    }
  }
  return declarations as PublishedDeclaration[];
}

// The declaration's references must resolve to globals, which the rest of
// the Workers types or TypeScript's libraries declare, or to published
// declarations: the bootstrap's other declarations do not reach the output.
// The program has no libraries, so a reference resolves only to one of the
// bootstrap's declaration files.
function checkReferences(
  checker: ts.TypeChecker,
  name: string,
  declaration: ts.Node,
  publishedSymbols: Set<ts.Symbol>
): void {
  const visit = (node: ts.Node): void => {
    let entity: ts.Node | undefined;
    if (ts.isTypeReferenceNode(node)) entity = node.typeName;
    else if (ts.isExpressionWithTypeArguments(node)) entity = node.expression;
    else if (ts.isTypeQueryNode(node)) entity = node.exprName;
    if (entity !== undefined) {
      checkEntity(checker, name, entity, publishedSymbols);
    }
    ts.forEachChild(node, visit);
  };
  visit(declaration);
}

function checkEntity(
  checker: ts.TypeChecker,
  name: string,
  entity: ts.Node,
  publishedSymbols: Set<ts.Symbol>
): void {
  // The leftmost identifier of `A.B.C` decides where the reference leads.
  let leftmost = entity;
  for (;;) {
    if (ts.isQualifiedName(leftmost)) leftmost = leftmost.left;
    else if (ts.isPropertyAccessExpression(leftmost)) {
      leftmost = leftmost.expression;
    } else break;
  }
  const local = checker.getSymbolAtLocation(leftmost);
  const isImport =
    local !== undefined && (local.flags & ts.SymbolFlags.Alias) !== 0;
  const symbol = isImport ? checker.getAliasedSymbol(local) : local;
  if (symbol !== undefined && publishedSymbols.has(symbol)) return;
  const declaration = symbol?.declarations?.[0];
  if (declaration === undefined) {
    // Unresolved: a global the bootstrap's declaration files do not declare,
    // unless it was imported from one.
    if (!isImport) return;
    throw new Error(
      `${name} refers to ${local.name}, whose import does not resolve to a declaration file`
    );
  }
  if (ts.isTypeParameterDeclaration(declaration)) return;
  throw new Error(
    `${name} refers to ${symbol.name}, which ${declaration.getSourceFile().fileName} declares but the bootstrap does not publish`
  );
}

// A published name's declarations as they print: a class absorbs the
// interfaces merged with it, and loses its `#private` marker; nothing is
// `export`ed.
function mergeDeclarations(
  name: string,
  declarations: PublishedDeclaration[]
): ts.Statement[] {
  const classes = declarations.filter(ts.isClassDeclaration);
  const interfaces = declarations.filter(ts.isInterfaceDeclaration);
  const aliases = declarations.filter(ts.isTypeAliasDeclaration);
  if (aliases.length > 0) {
    if (declarations.length > 1) {
      throw new Error(`${name} is a type alias with other declarations`);
    }
    const [alias] = aliases;
    return [
      f.updateTypeAliasDeclaration(
        alias,
        withoutExport(alias.modifiers),
        alias.name,
        alias.typeParameters,
        alias.type
      ),
    ];
  }
  if (classes.length > 1) {
    throw new Error(`${name} has more than one class declaration`);
  }
  if (classes.length === 0) {
    return interfaces.map((declaration) =>
      f.updateInterfaceDeclaration(
        declaration,
        withoutExport(declaration.modifiers),
        declaration.name,
        declaration.typeParameters,
        declaration.heritageClauses,
        declaration.members
      )
    );
  }
  return [foldInterfaces(name, classes[0], interfaces)];
}

// The class's declaration as a declaration file would write it with the
// interfaces' members: an interface merged with a class declares what the
// implementation installs on the prototype itself. An interface's `extends`
// becomes the class's, which must have none of its own.
function foldInterfaces(
  name: string,
  declaration: ts.ClassDeclaration,
  interfaces: ts.InterfaceDeclaration[]
): ts.ClassDeclaration {
  const members: ts.ClassElement[] = declaration.members.filter(
    (member) =>
      !(ts.isPropertyDeclaration(member) && ts.isPrivateIdentifier(member.name))
  );
  let heritageClauses: readonly ts.HeritageClause[] =
    declaration.heritageClauses ?? [];
  for (const merged of interfaces) {
    if (!sameTypeParameters(declaration, merged)) {
      throw new Error(
        `${name}: a merged interface must declare its class's type parameters`
      );
    }
    for (const clause of merged.heritageClauses ?? []) {
      const extendsClass = heritageClauses.some(
        (existing) => existing.token === ts.SyntaxKind.ExtendsKeyword
      );
      if (extendsClass || clause.types.length !== 1) {
        throw new Error(
          `${name}: a merged interface may give its class a base only if the class has none, and only one`
        );
      }
      heritageClauses = [
        f.createHeritageClause(ts.SyntaxKind.ExtendsKeyword, clause.types),
        ...heritageClauses,
      ];
    }
    for (const member of merged.members) {
      members.push(toClassElement(name, member));
    }
  }
  return f.updateClassDeclaration(
    declaration,
    withoutExport(declaration.modifiers),
    declaration.name,
    declaration.typeParameters,
    heritageClauses.length === 0 ? undefined : heritageClauses,
    members
  );
}

function typeParameterNames(
  parameters: ts.NodeArray<ts.TypeParameterDeclaration> | undefined
): string {
  if (parameters === undefined) return '';
  return parameters.map((parameter) => parameter.name.text).join(',');
}

function sameTypeParameters(
  declaration: ts.ClassDeclaration,
  merged: ts.InterfaceDeclaration
): boolean {
  return (
    typeParameterNames(declaration.typeParameters) ===
    typeParameterNames(merged.typeParameters)
  );
}

function toClassElement(name: string, member: ts.TypeElement): ts.ClassElement {
  let element: ts.ClassElement;
  if (ts.isMethodSignature(member)) {
    element = f.createMethodDeclaration(
      member.modifiers,
      /* asteriskToken */ undefined,
      member.name,
      member.questionToken,
      member.typeParameters,
      member.parameters,
      member.type,
      /* body */ undefined
    );
  } else if (ts.isPropertySignature(member)) {
    element = f.createPropertyDeclaration(
      member.modifiers,
      member.name,
      member.questionToken,
      member.type,
      /* initializer */ undefined
    );
  } else if (
    ts.isGetAccessorDeclaration(member) ||
    ts.isSetAccessorDeclaration(member) ||
    ts.isIndexSignatureDeclaration(member)
  ) {
    return member;
  } else {
    throw new Error(
      `${name}: a merged interface's ${ts.SyntaxKind[member.kind]} has no class equivalent`
    );
  }
  // Keeps the member's comments, which print from its source text.
  return ts.setOriginalNode(ts.setTextRange(element, member), member);
}

function withoutExport(
  modifiers: ts.NodeArray<ts.ModifierLike> | undefined
): ts.ModifierLike[] | undefined {
  return modifiers?.filter(
    (modifier) =>
      modifier.kind !== ts.SyntaxKind.ExportKeyword &&
      modifier.kind !== ts.SyntaxKind.DefaultKeyword
  );
}
