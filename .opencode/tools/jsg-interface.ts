import { tool } from '@opencode-ai/plugin';
import path from 'path';

export default tool({
  description:
    'Extract the full JSG interface for a C++ class — all methods, properties, constants, ' +
    'nested types, inheritance, iterators, serialization, and TypeScript overrides. ' +
    'Returns structured, categorized output instead of raw grep lines. ' +
    'Use this when you need to understand the complete JS-visible API of a workerd type.',
  args: {
    className: tool.schema
      .string()
      .describe(
        "C++ class name (e.g., 'ReadableStream', 'SubtleCrypto', 'Request')"
      ),
  },
  async execute(args, ctx) {
    const root = ctx.worktree || ctx.directory;
    const srcDir = path.join(root, 'src');
    const cls = args.className;

    // Step 1: Find the file containing JSG_RESOURCE_TYPE(cls) or JSG_STRUCT for this class
    let headerPath: string | null = null;
    let registrationType: 'resource' | 'struct' = 'resource';
    let allResourceFiles: { file: string; line: number }[] = [];

    try {
      const resourceHit =
        await Bun.$`grep -rn --include='*.h' "JSG_RESOURCE_TYPE.${cls}" ${srcDir}`.text();
      const hits = resourceHit.split('\n').filter(Boolean);
      // Pick the best match: prefer a file where JSG_RESOURCE_TYPE(ClassName is a top-level
      // class (not nested). Heuristic: the class declaration `class ClassName` appears in the
      // file without being inside another class's braces. Simple approach: prefer the file
      // where the line starts with exactly JSG_RESOURCE_TYPE(ClassName (allowing whitespace).
      // If ambiguous, return all.
      const parsed = hits
        .map((h) => {
          const m = h.match(/^(.+?):(\d+):(.*)$/);
          return m
            ? { file: m[1].trim(), line: parseInt(m[2]), text: m[3] }
            : null;
        })
        .filter(Boolean) as { file: string; line: number; text: string }[];

      if (parsed.length === 1) {
        headerPath = parsed[0].file;
      } else if (parsed.length > 1) {
        // Score each candidate: prefer files where the class is top-level (not nested)
        // Check if a `class ClassName` appears near the top of the file (not deep in another class)
        let best: (typeof parsed)[0] | null = null;
        let bestScore = -1;

        for (const p of parsed) {
          let score = 0;
          // Prefer files whose name contains the class name
          if (path.basename(p.file).toLowerCase().includes(cls.toLowerCase())) {
            score += 10;
          }
          // Check if `class ClassName` appears in the file as a top-level or first-level class
          // by reading a few lines before the JSG_RESOURCE_TYPE match
          try {
            const check =
              await Bun.$`grep -c "^class ${cls}\\b" ${p.file}`.text();
            const topLevelCount = parseInt(check.trim());
            // A top-level `class Foo` (no indentation) strongly suggests this is the primary class
            if (topLevelCount > 0) score += 20;
          } catch {
            // grep returned no matches
          }
          if (score > bestScore) {
            bestScore = score;
            best = p;
          }
        }

        headerPath = best ? best.file : parsed[0].file;
        // Store all files for reporting
        allResourceFiles = parsed.map((p) => ({
          file: path.relative(root, p.file),
          line: p.line,
        }));
      }
    } catch {
      // not a resource type, try struct
    }

    if (!headerPath) {
      try {
        const structHit =
          await Bun.$`grep -rnl --include='*.h' "JSG_STRUCT.*${cls}" ${srcDir}`.text();
        const files = structHit.split('\n').filter(Boolean);
        if (files.length > 0) {
          headerPath = files[0].trim();
          registrationType = 'struct';
        }
      } catch {
        // not found
      }
    }

    if (!headerPath) {
      return `No JSG registration found for \`${cls}\`. The class may not be a JSG-registered type, or it may use a different registration name.`;
    }

    // Step 2: Read the file
    const content = await Bun.file(headerPath).text();
    const relPath = path.relative(root, headerPath);

    if (registrationType === 'struct') {
      return extractStruct(content, cls, relPath);
    }

    return extractResourceType(content, cls, relPath, allResourceFiles);
  },
});

// --- Resource type extraction ---

function extractResourceType(
  content: string,
  cls: string,
  filePath: string,
  allFiles: { file: string; line: number }[] = []
): string {
  // Find the JSG_RESOURCE_TYPE block - match the opening and find the balanced closing brace
  const lines = content.split('\n');
  let startLine = -1;
  let configParam = '';

  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(
      new RegExp(`JSG_RESOURCE_TYPE\\s*\\(\\s*${cls}(?:\\s*,\\s*([^)]+))?\\)`)
    );
    if (m) {
      startLine = i;
      configParam = m[1]?.trim() || '';
      break;
    }
  }

  if (startLine === -1) {
    return `Found file \`${filePath}\` but could not locate \`JSG_RESOURCE_TYPE(${cls})\` block.`;
  }

  // Find the block — count braces to find the matching close
  const block = extractBracedBlock(lines, startLine);
  if (!block) {
    return `Found \`JSG_RESOURCE_TYPE(${cls})\` at \`${filePath}:${startLine + 1}\` but could not extract the block.`;
  }

  // Parse all macro calls from the block
  const iface = parseInterface(block);

  // Also check for JSG_MEMORY_INFO, iterator macros, etc. in the broader class
  const classExtras = extractClassExtras(content, cls);

  // Build output
  let out = `## JSG Interface: ${cls}\n\n`;
  out += `**File:** \`${filePath}:${startLine + 1}\`\n`;
  if (configParam) {
    out += `**Config:** \`${configParam}\` (conditional registration)\n`;
  }
  if (allFiles.length > 1) {
    const others = allFiles.filter((f) => f.file !== filePath);
    out += `**Also registered in:** ${others.map((f) => `\`${f.file}:${f.line}\``).join(', ')} (may be a nested class with the same name)\n`;
  }
  out += '\n';

  // Inheritance
  if (iface.inherits.length > 0) {
    out += `### Inheritance\n\n`;
    for (const i of iface.inherits) {
      out += `- ${i}\n`;
    }
    out += '\n';
  }

  // Methods
  if (iface.methods.length > 0) {
    out += `### Methods\n\n`;
    for (const m of iface.methods) {
      out += `- \`${m.jsName}\``;
      if (m.cppName !== m.jsName) out += ` → \`${m.cppName}\``;
      if (m.isStatic) out += ' *(static)*';
      out += '\n';
    }
    out += '\n';
  }

  // Properties
  if (iface.properties.length > 0) {
    out += `### Properties\n\n`;
    for (const p of iface.properties) {
      const flags: string[] = [];
      if (p.readonly) flags.push('readonly');
      if (p.lazy) flags.push('lazy');
      if (p.location === 'instance') flags.push('instance');
      if (p.location === 'prototype') flags.push('prototype');
      if (p.location === 'static') flags.push('static');
      if (p.location === 'inspect') flags.push('inspect-only');
      out += `- \`${p.jsName}\``;
      if (p.getter && p.getter !== p.jsName)
        out += ` → getter: \`${p.getter}\``;
      if (p.setter) out += `, setter: \`${p.setter}\``;
      if (flags.length > 0) out += ` *(${flags.join(', ')})*`;
      out += '\n';
    }
    out += '\n';
  }

  // Constants
  if (iface.constants.length > 0) {
    out += `### Constants\n\n`;
    for (const c of iface.constants) {
      out += `- \`${c.jsName}\``;
      if (c.cppExpr && c.cppExpr !== c.jsName) out += ` = \`${c.cppExpr}\``;
      out += '\n';
    }
    out += '\n';
  }

  // Nested types
  if (iface.nestedTypes.length > 0) {
    out += `### Nested Types\n\n`;
    for (const n of iface.nestedTypes) {
      out += `- \`${n.jsName}\``;
      if (n.cppType !== n.jsName) out += ` → \`${n.cppType}\``;
      out += '\n';
    }
    out += '\n';
  }

  // Iterators
  if (iface.iterators.length > 0) {
    out += `### Iterators\n\n`;
    for (const it of iface.iterators) {
      out += `- \`${it.kind}\` via \`${it.method}\`\n`;
    }
    out += '\n';
  }

  // Serialization
  if (iface.serialization) {
    out += `### Serialization\n\n`;
    out += `- ${iface.serialization}\n\n`;
  }

  // Callable
  if (iface.callable) {
    out += `### Callable\n\n`;
    out += `- Invocable via \`${iface.callable}\`\n\n`;
  }

  // Wildcard property
  if (iface.wildcardProperty) {
    out += `### Wildcard Property\n\n`;
    out += `- Catch-all getter: \`${iface.wildcardProperty}\`\n\n`;
  }

  // Dispose
  if (iface.dispose.length > 0) {
    out += `### Disposal\n\n`;
    for (const d of iface.dispose) {
      out += `- \`${d.kind}\` via \`${d.method}\`\n`;
    }
    out += '\n';
  }

  // TypeScript overrides
  if (iface.tsOverrides.length > 0) {
    out += `### TypeScript Overrides\n\n`;
    for (const ts of iface.tsOverrides) {
      out += `- \`${ts.kind}\`: ${ts.content.length > 80 ? ts.content.substring(0, 80) + '...' : ts.content}\n`;
    }
    out += '\n';
  }

  // JS bundles
  if (iface.jsBundles.length > 0) {
    out += `### JS Bundles\n\n`;
    for (const b of iface.jsBundles) {
      out += `- \`${b}\`\n`;
    }
    out += '\n';
  }

  // Class-level extras
  if (classExtras.hasMemoryInfo) {
    out += `**Memory tracking:** Has \`JSG_MEMORY_INFO\`\n`;
  }
  if (classExtras.iteratorDecls.length > 0) {
    out += `**Iterator declarations:**\n`;
    for (const it of classExtras.iteratorDecls) {
      out += `- \`${it}\`\n`;
    }
  }

  return out;
}

// --- Struct extraction ---

function extractStruct(content: string, cls: string, filePath: string): string {
  const lines = content.split('\n');
  let structLine = -1;
  let fields: string[] = [];

  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(/JSG_STRUCT\s*\(([^)]*)\)/);
    if (m && isInClass(content, cls, i)) {
      structLine = i;
      fields = m[1]
        .split(',')
        .map((f) => f.trim())
        .filter(Boolean);
      break;
    }
  }

  // Check for multi-line JSG_STRUCT
  if (structLine === -1) {
    for (let i = 0; i < lines.length; i++) {
      if (lines[i].includes('JSG_STRUCT(') && isInClass(content, cls, i)) {
        structLine = i;
        // Gather continuation lines
        let combined = '';
        for (let j = i; j < lines.length; j++) {
          combined += lines[j];
          if (combined.includes(')')) break;
        }
        const m = combined.match(/JSG_STRUCT\s*\(([^)]*)\)/);
        if (m) {
          fields = m[1]
            .split(',')
            .map((f) => f.trim())
            .filter(Boolean);
        }
        break;
      }
    }
  }

  let out = `## JSG Struct: ${cls}\n\n`;
  out += `**File:** \`${filePath}:${structLine + 1}\`\n`;
  out += `**Type:** Value type (deep-copied to/from JS objects)\n\n`;

  if (fields.length > 0) {
    out += `### Fields\n\n`;
    for (const f of fields) {
      const jsName = f.startsWith('$') ? f.substring(1) : f;
      out += `- \`${jsName}\``;
      if (f.startsWith('$')) out += ` (C++ name: \`${f}\`)`;
      out += '\n';
    }
    out += '\n';
  }

  // Check for TS overrides
  const tsLines = lines.filter(
    (l) =>
      l.includes('JSG_STRUCT_TS_OVERRIDE') || l.includes('JSG_STRUCT_TS_DEFINE')
  );
  if (tsLines.length > 0) {
    out += `### TypeScript Overrides\n\n`;
    for (const l of tsLines) {
      out += `- \`${l.trim()}\`\n`;
    }
  }

  return out;
}

// --- Parsing helpers ---

interface Method {
  jsName: string;
  cppName: string;
  isStatic: boolean;
}

interface Property {
  jsName: string;
  getter: string;
  setter: string;
  readonly: boolean;
  lazy: boolean;
  location: 'instance' | 'prototype' | 'static' | 'inspect';
}

interface Constant {
  jsName: string;
  cppExpr: string;
}

interface NestedType {
  jsName: string;
  cppType: string;
}

interface Iterator {
  kind: string;
  method: string;
}

interface Dispose {
  kind: string;
  method: string;
}

interface TsOverride {
  kind: string;
  content: string;
}

interface ParsedInterface {
  inherits: string[];
  methods: Method[];
  properties: Property[];
  constants: Constant[];
  nestedTypes: NestedType[];
  iterators: Iterator[];
  serialization: string | null;
  callable: string | null;
  wildcardProperty: string | null;
  dispose: Dispose[];
  tsOverrides: TsOverride[];
  jsBundles: string[];
}

function parseInterface(block: string): ParsedInterface {
  const result: ParsedInterface = {
    inherits: [],
    methods: [],
    properties: [],
    constants: [],
    nestedTypes: [],
    iterators: [],
    serialization: null,
    callable: null,
    wildcardProperty: null,
    dispose: [],
    tsOverrides: [],
    jsBundles: [],
  };

  // Normalize: join continuation lines (lines ending with \) and collapse multi-line macros
  const lines = block.split('\n');

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line || line.startsWith('//') || line.startsWith('/*')) continue;

    // JSG_INHERIT
    let m = line.match(/JSG_INHERIT\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.inherits.push(`JSG_INHERIT(${m[1]})`);
      continue;
    }
    m = line.match(/JSG_INHERIT_INTRINSIC\s*\(\s*([^)]+)\s*\)/);
    if (m) {
      result.inherits.push(`JSG_INHERIT_INTRINSIC(${m[1].trim()})`);
      continue;
    }

    // JSG_METHOD / JSG_METHOD_NAMED / JSG_STATIC_METHOD / JSG_STATIC_METHOD_NAMED
    m = line.match(/JSG_STATIC_METHOD_NAMED\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/);
    if (m) {
      result.methods.push({ jsName: m[1], cppName: m[2], isStatic: true });
      continue;
    }
    m = line.match(/JSG_STATIC_METHOD\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.methods.push({ jsName: m[1], cppName: m[1], isStatic: true });
      continue;
    }
    m = line.match(/JSG_METHOD_NAMED\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/);
    if (m) {
      result.methods.push({ jsName: m[1], cppName: m[2], isStatic: false });
      continue;
    }
    m = line.match(/JSG_METHOD\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.methods.push({ jsName: m[1], cppName: m[1], isStatic: false });
      continue;
    }

    // JSG_CALLABLE
    m = line.match(/JSG_CALLABLE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.callable = m[1];
      continue;
    }

    // Properties — order matters: match more specific patterns first
    // Static readonly
    m = line.match(
      /JSG_STATIC_READONLY_PROPERTY_NAMED\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: true,
        lazy: false,
        location: 'static',
      });
      continue;
    }
    m = line.match(/JSG_STATIC_READONLY_PROPERTY\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[1],
        setter: '',
        readonly: true,
        lazy: false,
        location: 'static',
      });
      continue;
    }

    // Lazy instance properties
    m = line.match(
      /JSG_LAZY_READONLY_INSTANCE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: true,
        lazy: true,
        location: 'instance',
      });
      continue;
    }
    m = line.match(/JSG_LAZY_INSTANCE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/);
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: false,
        lazy: true,
        location: 'instance',
      });
      continue;
    }

    // Instance properties
    m = line.match(
      /JSG_READONLY_INSTANCE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: true,
        lazy: false,
        location: 'instance',
      });
      continue;
    }
    m = line.match(
      /JSG_INSTANCE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: m[3],
        readonly: false,
        lazy: false,
        location: 'instance',
      });
      continue;
    }

    // Prototype properties
    m = line.match(
      /JSG_READONLY_PROTOTYPE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: true,
        lazy: false,
        location: 'prototype',
      });
      continue;
    }
    m = line.match(
      /JSG_PROTOTYPE_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)/
    );
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: m[3],
        readonly: false,
        lazy: false,
        location: 'prototype',
      });
      continue;
    }

    // Inspect property
    m = line.match(/JSG_INSPECT_PROPERTY\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/);
    if (m) {
      result.properties.push({
        jsName: m[1],
        getter: m[2],
        setter: '',
        readonly: true,
        lazy: false,
        location: 'inspect',
      });
      continue;
    }

    // Wildcard property
    m = line.match(/JSG_WILDCARD_PROPERTY\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.wildcardProperty = m[1];
      continue;
    }

    // Constants
    m = line.match(/JSG_STATIC_CONSTANT_NAMED\s*\(\s*(\w+)\s*,\s*([^)]+)\s*\)/);
    if (m) {
      result.constants.push({ jsName: m[1], cppExpr: m[2].trim() });
      continue;
    }
    m = line.match(/JSG_STATIC_CONSTANT\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.constants.push({ jsName: m[1], cppExpr: m[1] });
      continue;
    }

    // Nested types
    m = line.match(/JSG_NESTED_TYPE_NAMED\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)/);
    if (m) {
      result.nestedTypes.push({ cppType: m[1], jsName: m[2] });
      continue;
    }
    m = line.match(/JSG_NESTED_TYPE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.nestedTypes.push({ cppType: m[1], jsName: m[1] });
      continue;
    }

    // Iterators
    m = line.match(/JSG_ASYNC_ITERABLE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.iterators.push({ kind: '[Symbol.asyncIterator]', method: m[1] });
      continue;
    }
    m = line.match(/JSG_ITERABLE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.iterators.push({ kind: '[Symbol.iterator]', method: m[1] });
      continue;
    }

    // Dispose
    m = line.match(/JSG_ASYNC_DISPOSE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.dispose.push({ kind: '[Symbol.asyncDispose]', method: m[1] });
      continue;
    }
    m = line.match(/JSG_DISPOSE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.dispose.push({ kind: '[Symbol.dispose]', method: m[1] });
      continue;
    }

    // Serialization
    m = line.match(/JSG_ONEWAY_SERIALIZABLE\s*\(\s*([^)]+)\s*\)/);
    if (m) {
      result.serialization = `One-way serializable (tag: ${m[1].trim()})`;
      continue;
    }
    m = line.match(/JSG_SERIALIZABLE\s*\(\s*([^)]+)\s*\)/);
    if (m) {
      result.serialization = `Serializable (tag: ${m[1].trim()})`;
      continue;
    }

    // TypeScript overrides — these are multi-line, grab the macro name
    if (line.includes('JSG_TS_ROOT')) {
      result.tsOverrides.push({ kind: 'JSG_TS_ROOT', content: '(root type)' });
      continue;
    }
    m = line.match(/JSG_TS_OVERRIDE\s*\((.*)$/);
    if (m) {
      const tsContent = gatherMultiLineMacro(lines, i);
      result.tsOverrides.push({ kind: 'JSG_TS_OVERRIDE', content: tsContent });
      continue;
    }
    m = line.match(/JSG_TS_DEFINE\s*\((.*)$/);
    if (m) {
      const tsContent = gatherMultiLineMacro(lines, i);
      result.tsOverrides.push({ kind: 'JSG_TS_DEFINE', content: tsContent });
      continue;
    }

    // JS bundles
    m = line.match(/JSG_CONTEXT_JS_BUNDLE\s*\(\s*(\w+)\s*\)/);
    if (m) {
      result.jsBundles.push(m[1]);
      continue;
    }
  }

  return result;
}

function extractBracedBlock(lines: string[], startLine: number): string | null {
  // Find the first { on or after startLine
  let braceStart = -1;
  for (let i = startLine; i < Math.min(startLine + 5, lines.length); i++) {
    if (lines[i].includes('{')) {
      braceStart = i;
      break;
    }
  }
  if (braceStart === -1) return null;

  let depth = 0;
  let blockLines: string[] = [];
  for (let i = braceStart; i < lines.length; i++) {
    for (const ch of lines[i]) {
      if (ch === '{') depth++;
      if (ch === '}') depth--;
    }
    blockLines.push(lines[i]);
    if (depth === 0) break;
  }

  return blockLines.join('\n');
}

function gatherMultiLineMacro(lines: string[], startIdx: number): string {
  // Gather lines until parens balance
  let depth = 0;
  let result = '';
  for (let i = startIdx; i < lines.length; i++) {
    const line = lines[i];
    for (const ch of line) {
      if (ch === '(') depth++;
      if (ch === ')') depth--;
    }
    result += line.trim() + ' ';
    if (depth <= 0) break;
  }
  // Clean up: extract content between outer parens
  const m = result.match(/JSG_TS_\w+\s*\((.+)\)\s*;?\s*$/s);
  return m ? m[1].trim() : result.trim();
}

function isInClass(content: string, cls: string, lineIdx: number): boolean {
  // Simple heuristic: check if there's a `class/struct <cls>` before this line
  const lines = content.split('\n');
  for (let i = lineIdx - 1; i >= Math.max(0, lineIdx - 100); i--) {
    if (lines[i].match(new RegExp(`(?:class|struct)\\s+${cls}\\b`))) {
      return true;
    }
  }
  return false;
}

function extractClassExtras(
  content: string,
  cls: string
): { hasMemoryInfo: boolean; iteratorDecls: string[] } {
  const hasMemoryInfo = content.includes(`JSG_MEMORY_INFO(${cls})`);
  const iteratorDecls: string[] = [];

  const iteratorPatterns = [
    /JSG_ITERATOR\s*\(\s*\w+\s*,\s*\w+\s*,/g,
    /JSG_ASYNC_ITERATOR\s*\(\s*\w+\s*,\s*\w+\s*,/g,
    /JSG_ITERATOR_TYPE\s*\(\s*\w+\s*,/g,
    /JSG_ASYNC_ITERATOR_TYPE\s*\(\s*\w+\s*,/g,
  ];

  for (const pat of iteratorPatterns) {
    let m;
    while ((m = pat.exec(content)) !== null) {
      iteratorDecls.push(m[0].replace(/,\s*$/, ')'));
    }
  }

  return { hasMemoryInfo, iteratorDecls };
}
