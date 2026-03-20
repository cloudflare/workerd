import { tool } from '@opencode-ai/plugin';
import path from 'path';

export default tool({
  description:
    'One-shot cross-reference lookup for a C++ class or symbol in the workerd codebase. ' +
    'Returns header location, implementation file, JSG registration (methods/properties), ' +
    'EW_*_ISOLATE_TYPES group, test files, and compat flag gating — all in a single call. ' +
    'Saves 4-6 separate grep/read calls.',
  args: {
    symbol: tool.schema
      .string()
      .describe(
        "Class or symbol name (e.g., 'ReadableStream', 'SubtleCrypto', 'AbortSignal')"
      ),
  },
  async execute(args, ctx) {
    const root = ctx.worktree || ctx.directory;
    const sym = args.symbol;
    const srcDir = path.join(root, 'src');

    // Run all searches in parallel
    const [
      headerHits,
      implHits,
      jsgTypeHits,
      testHits,
      compatFlagHits,
      isolateTypeMacro,
    ] = await Promise.all([
      grepFiles(srcDir, `class ${sym}\\b`, '*.h'),
      grepFiles(srcDir, `${sym}::`, '*.c++'),
      grepFiles(srcDir, `JSG_RESOURCE_TYPE.${sym}`, '*.h'),
      grepTestFiles(srcDir, sym),
      grepFiles(srcDir, `flags.get.*().*${sym}\\|${sym}.*flags.get`, '*.h'),
      findIsolateTypeMacro(srcDir, sym),
    ]);

    // Extract JSG methods if we found the registration
    let jsgMethods: string[] = [];
    if (jsgTypeHits.length > 0) {
      jsgMethods = await extractJsgMethods(
        path.join(root, jsgTypeHits[0].file)
      );
    }

    // Build output
    let out = `## Cross-reference: ${sym}\n\n`;

    // Header
    const primaryHeader = findPrimary(headerHits, (text) =>
      /class\s+\w+\s*(final\s*)?[:{]/.test(text)
    );

    if (primaryHeader) {
      out += `### Declaration\n\n`;
      out += `- \`${primaryHeader.file}:${primaryHeader.line}\`: ${primaryHeader.text.trim()}\n`;
      const others = headerHits.filter(
        (h) => !(h.file === primaryHeader.file && h.line === primaryHeader.line)
      );
      if (others.length > 0) {
        out += `- Also referenced in: ${others
          .slice(0, 5)
          .map((h) => `\`${h.file}:${h.line}\``)
          .join(', ')}`;
        if (others.length > 5) out += ` (+${others.length - 5} more)`;
        out += '\n';
      }
    } else if (headerHits.length > 0) {
      out += `### Header references\n\n`;
      for (const h of headerHits.slice(0, 5)) {
        out += `- \`${h.file}:${h.line}\`: ${h.text.trim()}\n`;
      }
    } else {
      out += `### Declaration\n\nNot found in any .h file.\n`;
    }

    // Implementation
    out += `\n### Implementation\n\n`;
    if (implHits.length > 0) {
      const implFiles = [...new Set(implHits.map((h) => h.file))];
      for (const f of implFiles.slice(0, 5)) {
        const count = implHits.filter((h) => h.file === f).length;
        out += `- \`${f}\` (${count} references)\n`;
      }
    } else {
      out += `No .c++ files reference \`${sym}::\`.\n`;
    }

    // JSG registration
    out += `\n### JSG Registration\n\n`;
    if (jsgTypeHits.length > 0) {
      out += `- Registered in \`${jsgTypeHits[0].file}:${jsgTypeHits[0].line}\`\n`;
      if (jsgMethods.length > 0) {
        out += `- Methods/properties:\n`;
        for (const m of jsgMethods) {
          out += `  - \`${m}\`\n`;
        }
      }
    } else {
      out += `No \`JSG_RESOURCE_TYPE(${sym})\` found.\n`;
    }

    // Isolate type group
    out += `\n### Type Registration\n\n`;
    if (isolateTypeMacro) {
      out += `- Part of \`${isolateTypeMacro.macro}\` (defined in \`${isolateTypeMacro.file}:${isolateTypeMacro.line}\`)\n`;
    } else {
      out += `Not found in any \`EW_*_ISOLATE_TYPES\` macro.\n`;
    }

    // Compat flag gating
    if (compatFlagHits.length > 0) {
      out += `\n### Compat Flag Gating\n\n`;
      for (const h of compatFlagHits.slice(0, 5)) {
        out += `- \`${h.file}:${h.line}\`: ${h.text.trim()}\n`;
      }
    }

    // Tests
    out += `\n### Tests\n\n`;
    if (testHits.length > 0) {
      const testFiles = [...new Set(testHits.map((h) => h.file))];
      for (const f of testFiles.slice(0, 10)) {
        out += `- \`${f}\`\n`;
      }
      if (testFiles.length > 10) {
        out += `- ... and ${testFiles.length - 10} more\n`;
      }
    } else {
      out += `No test files found referencing \`${sym}\`.\n`;
    }

    return out;
  },
});

// --- grep helpers ---

interface Hit {
  file: string;
  line: number;
  text: string;
}

async function grepFiles(
  searchDir: string,
  pattern: string,
  includeGlob: string
): Promise<Hit[]> {
  try {
    const output =
      await Bun.$`grep -rnE --include=${includeGlob} -m 50 ${pattern} ${searchDir}`.text();
    return parseGrepOutput(output, path.dirname(searchDir)); // root = parent of src/
  } catch {
    return [];
  }
}

async function grepTestFiles(
  searchDir: string,
  symbol: string
): Promise<Hit[]> {
  try {
    // Match test files by name patterns
    const output =
      await Bun.$`grep -rnl --include='*test*.js' --include='*test*.ts' --include='*test*.c++' --include='*.wd-test' -m 50 ${symbol} ${searchDir}`.text();
    const root = path.dirname(searchDir);
    return output
      .split('\n')
      .filter(Boolean)
      .map((f) => ({
        file: path.relative(root, f.trim()),
        line: 0,
        text: '',
      }));
  } catch {
    return [];
  }
}

function parseGrepOutput(output: string, root: string): Hit[] {
  const hits: Hit[] = [];
  for (const line of output.split('\n')) {
    if (!line.trim()) continue;
    const m = line.match(/^(.+?):(\d+):(.*)$/);
    if (m) {
      hits.push({
        file: path.relative(root, m[1]),
        line: parseInt(m[2], 10),
        text: m[3],
      });
    }
  }
  return hits;
}

function findPrimary(
  hits: Hit[],
  predicate: (text: string) => boolean
): Hit | null {
  return hits.find((h) => predicate(h.text)) || hits[0] || null;
}

// --- JSG method extraction ---

async function extractJsgMethods(headerPath: string): Promise<string[]> {
  try {
    const output =
      await Bun.$`grep -E 'JSG_METHOD|JSG_READONLY_PROTOTYPE_PROPERTY|JSG_LAZY_INSTANCE_PROPERTY|JSG_STATIC_METHOD|JSG_NESTED_TYPE|JSG_INHERIT|JSG_TS_' ${headerPath}`.text();
    return output
      .split('\n')
      .map((l) => l.trim())
      .filter((l) => l.length > 0)
      .slice(0, 30);
  } catch {
    return [];
  }
}

// --- Isolate type macro lookup ---

async function findIsolateTypeMacro(
  searchDir: string,
  symbol: string
): Promise<{ macro: string; file: string; line: number } | null> {
  const root = path.dirname(searchDir);
  try {
    // Find files that define EW_*_ISOLATE_TYPES macros
    const macroFiles =
      await Bun.$`grep -rnl --include='*.h' 'EW_.*_ISOLATE_TYPES' ${searchDir}`.text();

    for (const file of macroFiles.split('\n').filter(Boolean)) {
      const filePath = file.trim();

      // Check if this file contains both the macro definition and the symbol
      let content: string;
      try {
        content = await Bun.$`grep -n ${symbol} ${filePath}`.text();
      } catch {
        continue; // symbol not in this file
      }
      if (!content.trim()) continue;

      // Find the EW_*_ISOLATE_TYPES definition line
      let macroDef: string;
      try {
        macroDef =
          await Bun.$`grep -n 'EW_.*_ISOLATE_TYPES' ${filePath}`.text();
      } catch {
        continue;
      }

      for (const defLine of macroDef.split('\n')) {
        const dm = defLine.match(/^(\d+):.*(EW_\w+_ISOLATE_TYPES)/);
        if (!dm) continue;

        // Check if symbol appears on the same line (inline macro)
        if (defLine.includes(symbol)) {
          return {
            macro: dm[2],
            file: path.relative(root, filePath),
            line: parseInt(dm[1], 10),
          };
        }

        // Check continuation lines: symbol within 80 lines of macro def
        const macroLine = parseInt(dm[1], 10);
        for (const symLine of content.split('\n')) {
          const sm = symLine.match(/^(\d+):/);
          if (!sm) continue;
          const symLineNum = parseInt(sm[1], 10);
          if (symLineNum >= macroLine && symLineNum <= macroLine + 80) {
            return {
              macro: dm[2],
              file: path.relative(root, filePath),
              line: macroLine,
            };
          }
        }
      }
    }
  } catch {
    // grep failed
  }
  return null;
}
