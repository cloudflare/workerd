import { tool } from '@opencode-ai/plugin';
import path from 'path';
import { runCapnpcCapnp, resolveCapnpPath } from './capnp.ts';

export default tool({
  description:
    "Find the next available field ordinal (@N) in a Cap'n Proto struct. " +
    'Uses the capnp compiler (capnpc-capnp) to parse the schema canonically. ' +
    'Use before adding new fields to .capnp files (especially compatibility-date.capnp) ' +
    'to avoid ordinal collisions.',
  args: {
    file: tool.schema
      .string()
      .describe('Path to .capnp file (absolute, or relative to project root)'),
    struct: tool.schema
      .string()
      .optional()
      .describe(
        "Struct name (e.g., 'CompatibilityFlags'). Omit to list all structs."
      ),
  },
  async execute(args, ctx) {
    const root = ctx.worktree || ctx.directory;
    const filePath = resolveCapnpPath(root, args.file);

    const result = await runCapnpcCapnp(root, filePath);
    if (typeof result !== 'string') return result.error;

    const structs = parseCapnpOutput(result);

    if (!args.struct) {
      return listStructs(structs, filePath);
    }

    // Support dotted names like "Worker.Module"
    const target =
      structs.find((s) => s.qualifiedName === args.struct) ||
      structs.find((s) => s.name === args.struct);

    if (!target) {
      const available = structs.map((s) => s.qualifiedName).join(', ');
      return `Struct '${args.struct}' not found in ${path.basename(filePath)}\n\nAvailable: ${available}`;
    }

    return formatResult(target, filePath);
  },
});

// --- Output parsing ---

interface FieldInfo {
  name: string;
  ordinal: number;
  type: string;
  annotations: string[];
  raw: string;
}

interface StructInfo {
  name: string;
  qualifiedName: string;
  fields: FieldInfo[];
  declaredAnnotations: string[];
}

function parseCapnpOutput(output: string): StructInfo[] {
  const lines = output.split('\n');
  const allStructs: StructInfo[] = [];

  const nameStack: string[] = [];
  let currentStruct: StructInfo | null = null;

  let depth = 0;
  const structDepths: number[] = [];

  for (const line of lines) {
    const opens = (line.match(/\{/g) || []).length;
    const closes = (line.match(/\}/g) || []).length;

    // Check for struct opening
    const structMatch = line.match(/struct\s+(\w+)\s+@0x[\da-fA-F]+\s*\{/);
    if (structMatch) {
      if (currentStruct) {
        allStructs.push(currentStruct);
      }

      const name = structMatch[1];
      nameStack.push(name);
      structDepths.push(depth);

      currentStruct = {
        name,
        qualifiedName: nameStack.join('.'),
        fields: [],
        declaredAnnotations: [],
      };

      depth += opens;
      continue;
    }

    depth += opens;

    // Field declarations: "  fieldName @N :Type ..."
    const fieldMatch = line.match(/^\s+(\w+)\s+@(\d+)\s+:(\S+)/);
    if (fieldMatch && currentStruct) {
      const annotations: string[] = [];
      const annoRe = /\$(\w+)\(([^)]*)\)/g;
      let m;
      while ((m = annoRe.exec(line)) !== null) {
        annotations.push(`$${m[1]}(${m[2]})`);
      }

      const raw = line
        .trim()
        .replace(/\s+#\s.*$/, '')
        .replace(/;$/, '');

      currentStruct.fields.push({
        name: fieldMatch[1],
        ordinal: parseInt(fieldMatch[2], 10),
        type: fieldMatch[3].replace(/[;,]$/, ''),
        annotations,
        raw,
      });
    }

    // Annotation declarations inside a struct
    const annoDecl = line.match(/^\s+annotation\s+(\w+)\s+@0x/);
    if (annoDecl && currentStruct) {
      currentStruct.declaredAnnotations.push(annoDecl[1]);
    }

    depth -= closes;

    // Check if we've closed a struct
    while (
      structDepths.length > 0 &&
      depth <= structDepths[structDepths.length - 1]
    ) {
      if (currentStruct) {
        allStructs.push(currentStruct);
        currentStruct = null;
      }
      structDepths.pop();
      nameStack.pop();

      if (nameStack.length > 0) {
        const parentName = nameStack.join('.');
        const parentIdx = allStructs.findIndex(
          (s) => s.qualifiedName === parentName
        );
        if (parentIdx >= 0) {
          currentStruct = allStructs[parentIdx];
          allStructs.splice(parentIdx, 1);
        }
      }
    }
  }

  if (currentStruct) {
    allStructs.push(currentStruct);
  }

  return allStructs;
}

// --- Formatting ---

function listStructs(structs: StructInfo[], filePath: string): string {
  if (structs.length === 0) {
    return `No structs found in ${path.basename(filePath)}`;
  }
  let result = `## Structs in ${path.basename(filePath)}\n\n`;
  for (const s of structs) {
    if (s.fields.length === 0) {
      result += `- **${s.qualifiedName}**: no fields\n`;
    } else {
      const max = Math.max(...s.fields.map((f) => f.ordinal));
      result += `- **${s.qualifiedName}**: ${s.fields.length} fields, highest @${max}, next @${max + 1}\n`;
    }
  }
  return result;
}

function formatResult(target: StructInfo, filePath: string): string {
  if (target.fields.length === 0) {
    return `Struct '${target.qualifiedName}' in ${path.basename(filePath)} has no fields.`;
  }

  const sorted = [...target.fields].sort((a, b) => a.ordinal - b.ordinal);
  const maxOrdinal = sorted[sorted.length - 1].ordinal;
  const nextOrdinal = maxOrdinal + 1;

  const used = new Set(target.fields.map((f) => f.ordinal));
  const gaps: number[] = [];
  for (let i = 0; i <= maxOrdinal; i++) {
    if (!used.has(i)) gaps.push(i);
  }

  const lastFields = sorted.slice(-5);

  let out = `## ${target.qualifiedName} in ${path.basename(filePath)}\n\n`;
  out += `**Next available ordinal: @${nextOrdinal}**\n\n`;
  out += `- Total fields: ${target.fields.length}\n`;
  out += `- Highest ordinal: @${maxOrdinal} (\`${sorted[sorted.length - 1].name}\`)\n`;

  if (gaps.length > 0) {
    out += `- Gaps: ${gaps.map((g) => `@${g}`).join(', ')}\n`;
    out += `  (Likely obsolete -- do NOT reuse)\n`;
  }

  if (target.declaredAnnotations.length > 0) {
    out += `- Annotations: ${target.declaredAnnotations.join(', ')}\n`;
  }

  out += `\n### Last ${lastFields.length} fields\n\n`;
  for (const f of lastFields) {
    out += `- \`${f.raw}\`\n`;
  }

  out += `\n### Usage\n\n`;
  out += '```capnp\n';
  out += `  yourNewField @${nextOrdinal} :Bool\n`;
  out += '```\n';

  return out;
}
