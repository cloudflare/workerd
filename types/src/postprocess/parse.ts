// Generate exportable and ambient version of the types
// Attach comments from lib.webworker (or whatever standards d.ts file is passed in)
// Note: assumptions are made about the shape of the standard types (specifically around how classes are defined)
import assert from "assert";
import { appendFile, readFile, writeFile } from "fs/promises";
import * as ts from "typescript";

interface TypeDefinition {
  path: string;
  program: ts.Program;
  source: ts.SourceFile;
  checker: ts.TypeChecker;
  log: (node: ts.Node) => string;
}

interface ParsedTypeDefinition extends TypeDefinition {
  parsed: {
    functions: Map<string, ts.FunctionDeclaration>;
    interfaces: Map<string, ts.InterfaceDeclaration>;
    vars: Map<string, ts.VariableDeclaration>;
    types: Map<string, ts.TypeAliasDeclaration>;
    classes: Map<string, ts.ClassDeclaration>;
  };
}
function attachComments(
  from: ts.Node,
  to: ts.Node,
  sourceFile: ts.SourceFile
): void {
  const text = sourceFile.getFullText();
  const extractCommentText = (comment: ts.CommentRange) =>
    comment.kind === ts.SyntaxKind.MultiLineCommentTrivia
      ? text.slice(comment.pos + 2, comment.end - 2)
      : text.slice(comment.pos + 2, comment.end);
  const leadingComments: string[] =
    ts
      .getLeadingCommentRanges(text, from.getFullStart())
      ?.map(extractCommentText) ?? [];

  leadingComments.map((c) =>
    ts.addSyntheticLeadingComment(
      to,
      ts.SyntaxKind.MultiLineCommentTrivia,
      c,
      true
    )
  );
}
function inflateTypes(
  types: Record<string, Pick<TypeDefinition, "path">>
): Record<string, TypeDefinition> {
  const printer = ts.createPrinter({ newLine: ts.NewLineKind.LineFeed });
  return Object.fromEntries(
    Object.entries(types).map(([label, definition]) => {
      const program = ts.createProgram(
        [definition.path],
        {},
        ts.createCompilerHost({}, true)
      );
      const source = program.getSourceFile(definition.path)!;
      const checker = program.getTypeChecker();
      return [
        label,
        {
          path: definition.path,
          program,
          source,
          checker,
          log: (node) =>
            printer.printNode(ts.EmitHint.Unspecified, node, source),
        },
      ];
    })
  );
}
function parseTypes(
  types: Record<string, TypeDefinition>
): Record<string, ParsedTypeDefinition> {
  return Object.fromEntries(
    Object.entries(types).map(([label, definition]) => {
      const parsed = {
        functions: new Map<string, ts.FunctionDeclaration>(),
        interfaces: new Map<string, ts.InterfaceDeclaration>(),
        vars: new Map<string, ts.VariableDeclaration>(),
        types: new Map<string, ts.TypeAliasDeclaration>(),
        classes: new Map<string, ts.ClassDeclaration>(),
      };
      ts.forEachChild(definition.source, (node) => {
        let name = "";
        if (node && ts.isFunctionDeclaration(node)) {
          name = node.name?.text ?? "";
          parsed.functions.set(name, node);
        } else if (ts.isVariableStatement(node)) {
          name = node.declarationList.declarations[0].name.getText(
            definition.source
          );
          assert(node.declarationList.declarations.length === 1);
          parsed.vars.set(name, node.declarationList.declarations[0]);
        } else if (ts.isInterfaceDeclaration(node)) {
          name = node.name.text;
          parsed.interfaces.set(name, node);
        } else if (ts.isTypeAliasDeclaration(node)) {
          name = node.name.text;
          parsed.types.set(name, node);
        } else if (ts.isClassDeclaration(node)) {
          name = node.name?.text ?? "";
          parsed.classes.set(name, node);
        }
      });
      return [
        label,
        {
          ...definition,
          parsed,
        },
      ];
    })
  );
}

function getNames(
  type: ParsedTypeDefinition,
  types: (keyof ParsedTypeDefinition["parsed"])[] = [
    "interfaces",
    "classes",
    "functions",
    "vars",
    "types",
  ]
): string[] {
  return types.flatMap((k) => [...type.parsed[k].keys()]);
}

function simpleDiff(
  first: string[],
  second: string[]
): {
  union: string[];
  added: string[];
  removed: string[];
} {
  return {
    union: second.filter((el) => first.includes(el)),
    added: second.filter((el) => !first.includes(el)),
    removed: first.filter((el) => !second.includes(el)),
  };
}

export default async function main(
  workersTypes: string,
  ...standardTypes: string[]
) {
  // Collate standards (to support lib.(dom|webworker).iterable.d.ts being defined separately)
  const STANDARDS_PATH = "./tmp.standards.d.ts";
  await writeFile(STANDARDS_PATH, "");
  await Promise.all(
    standardTypes.map(
      async (s) =>
        await appendFile(
          STANDARDS_PATH,
          // Remove the Microsoft copyright notices from the file, to prevent them being copied in as TS comments
          (await readFile(s, "utf-8")).split(`/////////////////////////////`)[2]
        )
    )
  );
  const types: Record<string, ParsedTypeDefinition> = parseTypes(
    inflateTypes({
      workers: { path: workersTypes },
      standards: { path: STANDARDS_PATH },
    })
  );
  class Property {
    public isStatic: boolean;
    public name: string;
    public node: ts.Node;
    constructor(isStatic: boolean, name: string, node: ts.Node) {
      this.isStatic = isStatic;
      this.name = name;
      this.node = node;
    }
    public getType(className: string, namespace: "DOM" | "CfWorker"): string {
      return this.isStatic
        ? `typeof ${namespace}.${className}.${this.name}`
        : `${namespace}.${className}["${this.name}"]`;
    }
    public getPath(className: string): string {
      return this.isStatic
        ? `${className}.${this.name}`
        : `${className}.prototype.${this.name}`;
    }
    public static makeStatic(property: Property, isStatic = true): Property {
      return new Property(isStatic, property.name, property.node);
    }
  }
  class Parameter extends Property {
    public parent: string;
    public isConstructor: boolean;
    constructor(property: Property, parent: string, isConstructor: boolean) {
      super(property.isStatic, property.name, property.node);
      this.parent = parent;
      this.isConstructor = isConstructor;
    }

    public getType(
      className: string,
      namespace: "DOM" | "CfWorker",
      index = 0
    ): string {
      const baseFunctionType = this.isStatic
        ? `typeof ${namespace}.${className}.${this.parent}`
        : `${namespace}.${className}["${this.parent}"]`;
      return this.isConstructor
        ? `ConstructorParameters<typeof ${namespace}.${className}>[${index}]`
        : `Parameters<${baseFunctionType}>[${index}]`;
    }
    public getPath(className: string): string {
      if (this.isConstructor) {
        return `new ${className}(${this.name})`;
      }
      return this.isStatic
        ? `${className}.${this.parent}(${this.name})`
        : `${className}.prototype.${this.parent}(${this.name})`;
    }
  }
  class DefinedFunction {
    isStatic: boolean;
    name: string;
    node: ts.Node;
    parameters: Parameter[];
    constructor(
      isStatic: boolean,
      name: string,
      node: ts.Node,
      parameters: Parameter[]
    ) {
      this.isStatic = isStatic;
      this.name = name;
      this.node = node;
      this.parameters = parameters;
    }
    public getType(className: string, namespace: "DOM" | "CfWorker"): string {
      return this.isStatic
        ? `typeof ${namespace}.${className}.${this.name}`
        : `${namespace}.${className}["${this.name}"]`;
    }
    public getPath(className: string): string {
      return this.isStatic
        ? `${className}.${this.name}()`
        : `${className}.prototype.${this.name}()`;
    }
    public static makeStatic(
      fn: DefinedFunction,
      isStatic = true
    ): DefinedFunction {
      return new DefinedFunction(isStatic, fn.name, fn.node, fn.parameters);
    }
  }

  abstract class DefinedClass {
    protected name: string;
    constructor(name: string) {
      this.name = name;
    }
    abstract get properties(): Property[];
    abstract get functions(): DefinedFunction[];
    abstract get construct(): DefinedFunction;

    protected hasStaticKeyword(node: ts.Node) {
      return !!node.modifiers?.find(
        (m) => m.kind === ts.SyntaxKind.StaticKeyword
      );
    }

    protected nodeToParameter(
      node:
        | ts.PropertyDeclaration
        | ts.ClassElement
        | ts.ParameterDeclaration
        | ts.TypeElement
    ): Property {
      assert(node?.name?.getText(), `Property with no name encountered`);

      return new Property(
        this.hasStaticKeyword(node),
        node.name!.getText(),
        node
      );
    }
    public abstract isMethod(node: ts.Node): boolean;
    public abstract isConstructor(node: ts.Node): boolean;

    public isFunction(node: ts.Node) {
      return !this.isConstructor(node) && this.isMethod(node);
    }

    public isProperty(node: ts.Node) {
      return !this.isConstructor(node) && !this.isMethod(node);
    }

    protected abstract getParameters(node: ts.Node): Property[];
  }

  class CfWorkerClass extends DefinedClass {
    private cls: ts.ClassDeclaration;
    constructor(name: string, cls: ts.ClassDeclaration) {
      super(name);
      this.cls = cls;
    }
    get construct(): DefinedFunction {
      const constructors = this.cls.members.filter((m) =>
        this.isConstructor(m)
      );
      assert(constructors.length > 0);
      assert(constructors.every(ts.isConstructorDeclaration));

      const args = Math.max(...constructors.map((c) => c.parameters.length));
      return new DefinedFunction(true, "constructor", constructors[0], [
        ...constructors
          .find((c) => c.parameters.length === args)!
          .parameters.map(
            (p) => new Parameter(this.nodeToParameter(p), "constructor", true)
          ),
      ]);
    }

    public isConstructor(node: ts.ClassElement): boolean {
      return ts.isConstructorDeclaration(node);
    }

    public isMethod(node: ts.ClassElement) {
      return (
        ts.isMethodDeclaration(node) ||
        (ts.isPropertyDeclaration(node) && ts.isFunctionLike(node.type))
      );
    }

    protected getParameters(node: ts.ClassElement): Property[] {
      let parameters: Property[];
      if (ts.isPropertyDeclaration(node)) {
        assert(ts.isFunctionLike(node.type));
        parameters = node.type.parameters.map((p) => this.nodeToParameter(p));
      } else {
        assert(ts.isMethodDeclaration(node));
        parameters = node.parameters.map((p) => this.nodeToParameter(p));
      }
      return parameters;
    }

    get functions(): DefinedFunction[] {
      return this.cls.members
        .filter((m) => this.isFunction(m))
        .map((p) => {
          const asProperty = this.nodeToParameter(p);
          const parameters = this.getParameters(p);
          return new DefinedFunction(
            asProperty.isStatic,
            asProperty.name,
            asProperty.node,
            parameters.map((p) => new Parameter(p, asProperty.name, false))
          );
        });
    }

    get properties(): Property[] {
      return this.cls.members
        .filter((n) => this.isProperty(n))
        .map((n) => this.nodeToParameter(n));
    }
  }

  class StandardsClass extends DefinedClass {
    get construct(): DefinedFunction {
      assert(
        ts.isTypeLiteralNode(this.variable.type!),
        `Non type literal found for "${this.name}"`
      );
      const constructors = this.variable.type.members.filter((m) =>
        this.isConstructor(m)
      );
      assert(constructors.length > 0);
      assert(constructors.every(ts.isConstructSignatureDeclaration));

      const args = Math.max(...constructors.map((c) => c.parameters.length));
      return new DefinedFunction(true, "constructor", constructors[0], [
        ...constructors
          .find((c) => c.parameters.length === args)!
          .parameters.map(
            (p) => new Parameter(this.nodeToParameter(p), "constructor", true)
          ),
      ]);
    }
    public isConstructor(node: ts.Node): boolean {
      return ts.isConstructSignatureDeclaration(node);
    }
    public isMethod(node: ts.Node): boolean {
      return (
        ts.isMethodSignature(node) ||
        (ts.isPropertySignature(node) && ts.isFunctionLike(node.type))
      );
    }
    protected getParameters(node: ts.Node): Property[] {
      let parameters: Property[];

      if (ts.isPropertySignature(node)) {
        assert(ts.isFunctionLike(node.type));
        parameters = node.type.parameters.map((m) => this.nodeToParameter(m));
      } else {
        assert(ts.isMethodSignature(node));
        parameters = node.parameters.map((m) => this.nodeToParameter(m));
      }
      return parameters;
    }
    private inter: ts.InterfaceDeclaration;
    private variable: ts.VariableDeclaration;
    constructor(
      name: string,
      inter: ts.InterfaceDeclaration,
      variable: ts.VariableDeclaration
    ) {
      super(name);
      this.inter = inter;
      this.variable = variable;
    }
    get functions(): DefinedFunction[] {
      assert(
        ts.isTypeLiteralNode(this.variable.type!),
        `Non type literal found for "${this.name}"`
      );
      const statics = this.variable.type.members.filter((m) =>
        this.isFunction(m)
      );
      const instance = this.inter.members.filter((m) => this.isFunction(m));
      return [
        ...statics.map((s) => {
          const asProperty = this.nodeToParameter(s);
          return new DefinedFunction(
            true,
            asProperty.name,
            asProperty.node,
            this.getParameters(s).map(
              (p) => new Parameter(p, asProperty.name, false)
            )
          );
        }),
        ...instance.map((s) => {
          const asProperty = this.nodeToParameter(s);
          return new DefinedFunction(
            false,
            asProperty.name,
            asProperty.node,
            this.getParameters(s).map(
              (p) => new Parameter(p, asProperty.name, false)
            )
          );
        }),
      ];
    }
    get properties(): Property[] {
      assert(
        ts.isTypeLiteralNode(this.variable.type!),
        `Non type literal found for "${this.name}"`
      );
      const statics = this.variable.type.members.filter(
        (n) => this.isProperty(n) && n.name?.getText() !== "prototype"
      );
      const instance = this.inter.members.filter((n) => this.isProperty(n));
      return [
        ...statics.map((s) => Property.makeStatic(this.nodeToParameter(s))),
        ...instance.map((s) =>
          Property.makeStatic(this.nodeToParameter(s), false)
        ),
      ];
    }
  }

  function zipObject<T extends { name: string }>(
    standards: T[],
    workers: T[]
  ): [T | undefined, T | undefined][] {
    const standardsObj = Object.fromEntries(
      standards.map((el) => [el.name, el])
    );
    const workersObj = Object.fromEntries(workers.map((el) => [el.name, el]));
    const { union, added, removed } = simpleDiff(
      Object.keys(standardsObj),
      Object.keys(workersObj)
    );
    return [
      ...union.map<[T, T]>((k) => [standardsObj[k], workersObj[k]]),
      ...removed.map<[T, undefined]>((k) => [standardsObj[k], undefined]),
      ...added.map<[undefined, T]>((k) => [undefined, workersObj[k]]),
    ];
  }
  function zip<T>(
    standards: T[],
    workers: T[]
  ): [T | undefined, T | undefined][] {
    if (standards.length > workers.length) {
      return standards.map((s, i) => [s, workers[i]]);
    } else {
      return workers.map((w, i) => [standards[i], w]);
    }
  }

  // This was originally using `appendFile`, which is why it's a slightly weird setup
  const out = {
    ambient: "",
    exportable: "",
  };

  function ambient(node: ts.Node): void {
    // @ts-ignore next-line
    node.modifiers = node.modifiers?.filter(
      (m) => m.kind !== ts.SyntaxKind.ExportKeyword
    );
    out.ambient += `${types.workers.log(node)}\n`;
  }
  function exportable(node: ts.Node): void {
    if (!node.modifiers?.find((m) => m.kind == ts.SyntaxKind.ExportKeyword)) {
      // @ts-ignore next-line
      node.modifiers = [
        ts.factory.createModifier(ts.SyntaxKind.ExportKeyword),
        ...(node.modifiers ?? []),
      ];
    }
    out.exportable += `${types.workers.log(node)}\n`;
  }
  function set(node: ts.Node) {
    ambient(node);
    exportable(node);
  }

  for (const className of getNames(types.workers, ["classes"])) {
    // No linked class found in standards
    if (
      !types.standards.parsed.interfaces.has(className) ||
      !types.standards.parsed.vars.has(className)
    ) {
      set(types.workers.parsed.classes.get(className)!);
      continue;
    }
    assert(
      types.standards.parsed.interfaces.has(className),
      "No interface defined"
    );
    const definitions = {
      var: types.standards.parsed.vars.get(className)!,
      interface: types.standards.parsed.interfaces.get(className)!,
      cls: types.workers.parsed.classes.get(className)!,
    };

    const worker = new CfWorkerClass(className, definitions.cls);
    const standard = new StandardsClass(
      className,
      definitions.interface,
      definitions.var
    );
    attachComments(definitions.var, definitions.cls, types.standards.source);
    attachComments(
      definitions.interface,
      definitions.cls,
      types.standards.source
    );

    zipObject(standard.properties, worker.properties).map(([s, w]) => {
      if (s && w) {
        attachComments(s.node, w.node, types.standards.source);
      }
    });
    zipObject(standard.functions, worker.functions).map(([s, w]) => {
      if (s && w) {
        attachComments(s.node, w.node, types.standards.source);
      }
    });
    set(definitions.cls);
  }

  for (const functionName of getNames(types.workers, ["functions"])) {
    if (!types.standards.parsed.functions.has(functionName)) {
      set(types.workers.parsed.functions.get(functionName)!);
      continue;
    }
    attachComments(
      types.standards.parsed.functions.get(functionName)!,
      types.workers.parsed.functions.get(functionName)!,
      types.standards.source
    );
    set(types.workers.parsed.functions.get(functionName)!);
  }

  for (const typeAlias of getNames(types.workers, ["types"])) {
    if (!types.standards.parsed.types.has(typeAlias)) {
      set(types.workers.parsed.types.get(typeAlias)!);
      continue;
    }
    attachComments(
      types.standards.parsed.types.get(typeAlias)!,
      types.workers.parsed.types.get(typeAlias)!,
      types.standards.source
    );
    set(types.workers.parsed.types.get(typeAlias)!);
  }

  const nameAlias = new Map([["RequestInitializerDict", "RequestInit"]]);
  for (const interfaceName of getNames(types.workers, ["interfaces"])) {
    const standardInterface = nameAlias.has(interfaceName)
      ? types.standards.parsed.interfaces.get(nameAlias.get(interfaceName)!)
      : types.standards.parsed.interfaces.get(interfaceName);
    if (!standardInterface) {
      set(types.workers.parsed.interfaces.get(interfaceName)!);
      continue;
    }
    attachComments(
      standardInterface,
      types.workers.parsed.interfaces.get(interfaceName)!,
      types.standards.source
    );

    zipObject(
      [...standardInterface.members].map((m) => ({
        node: m,
        name: m.name!.getText()!,
      })),
      [...types.workers.parsed.interfaces.get(interfaceName)!.members].map(
        (m) => ({ node: m, name: m.name!.getText()! })
      )
    ).forEach(([s, w]) => {
      if (s && w) attachComments(s.node, w.node, types.standards.source);
    });
    set(types.workers.parsed.interfaces.get(interfaceName)!);
  }
  const ignoreForComments = new Set(["self", "navigator", "caches"]);
  for (const variableName of getNames(types.workers, ["vars"])) {
    const statement = ts.factory.createVariableStatement(
      [
        ts.factory.createModifier(ts.SyntaxKind.ExportKeyword),
        ts.factory.createModifier(ts.SyntaxKind.DeclareKeyword),
      ],
      [types.workers.parsed.vars.get(variableName)!]
    );

    if (
      !types.standards.parsed.vars.has(variableName) ||
      ignoreForComments.has(variableName)
    ) {
      set(statement);
      continue;
    }
    attachComments(
      // Get the variable statement this declaration is part of
      types.standards.parsed.vars.get(variableName)!.parent.parent,
      statement,
      types.standards.source
    );
    set(statement);
  }
  return out;
}
