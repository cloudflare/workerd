import { tool } from '@opencode-ai/plugin';
import path from 'path';
import { runCapnpcCapnp, resolveCapnpPath } from './capnp.ts';

export default tool({
  description:
    'List which compatibility flags are active or inactive at a given date. ' +
    'Parses compatibility-date.capnp via the capnp compiler. Useful for understanding ' +
    'what behavior a worker gets at a specific compat date, debugging test variant ' +
    'failures (@, @all-compat-flags, @all-autogates), and writing .wd-test configs.',
  args: {
    date: tool.schema
      .string()
      .optional()
      .describe(
        'Compatibility date (YYYY-MM-DD). Omit to list all flags with their enable dates.'
      ),
    flag: tool.schema
      .string()
      .optional()
      .describe(
        'Filter by flag name substring (matches enable flag, disable flag, or field name)'
      ),
  },
  async execute(args, ctx) {
    const root = ctx.worktree || ctx.directory;
    const flags = await parseCompatFlags(root);
    if (typeof flags === 'string') return flags;

    let filtered = flags;

    if (args.flag) {
      const q = args.flag.toLowerCase();
      filtered = flags.filter(
        (f) =>
          f.fieldName.toLowerCase().includes(q) ||
          f.enableFlag?.toLowerCase().includes(q) ||
          f.disableFlag?.toLowerCase().includes(q)
      );
      if (filtered.length === 0) {
        return `No flags matching '${args.flag}'. There are ${flags.length} total flags.`;
      }
    }

    if (!args.date) {
      return formatAllFlags(filtered);
    }

    if (!/^\d{4}-\d{2}-\d{2}$/.test(args.date)) {
      return `Invalid date format: '${args.date}'. Use YYYY-MM-DD.`;
    }

    return formatAtDate(filtered, args.date);
  },
});

// --- Types ---

interface CompatFlag {
  fieldName: string;
  ordinal: number;
  enableFlag: string | null;
  disableFlag: string | null;
  enableDate: string | null;
  enableAllDates: boolean;
  experimental: boolean;
  neededByFl: boolean;
  impliedBy: string | null;
  obsolete: boolean;
}

// --- Parsing ---

const COMPAT_CAPNP = 'src/workerd/io/compatibility-date.capnp';

async function parseCompatFlags(root: string): Promise<CompatFlag[] | string> {
  const filePath = resolveCapnpPath(root, COMPAT_CAPNP);
  const result = await runCapnpcCapnp(root, filePath);
  if (typeof result !== 'string') return result.error;

  const flags: CompatFlag[] = [];

  for (const line of result.split('\n')) {
    const m = line.match(/^\s+(\w+)\s+@(\d+)\s+:Bool/);
    if (!m) continue;

    const fieldName = m[1];
    const ordinal = parseInt(m[2], 10);

    flags.push({
      fieldName,
      ordinal,
      enableFlag: extractAnno(line, 'compatEnableFlag'),
      disableFlag: extractAnno(line, 'compatDisableFlag'),
      enableDate: extractAnno(line, 'compatEnableDate'),
      enableAllDates: line.includes('$compatEnableAllDates('),
      experimental: line.includes('$experimental('),
      neededByFl: line.includes('$neededByFl('),
      impliedBy: extractRawAnno(line, 'impliedByAfterDate'),
      obsolete: fieldName.startsWith('obsolete'),
    });
  }

  return flags;
}

function extractAnno(line: string, name: string): string | null {
  const m = line.match(new RegExp(`\\$${name}\\("([^"]*)"\\)`));
  return m ? m[1] : null;
}

function extractRawAnno(line: string, name: string): string | null {
  const m = line.match(new RegExp(`\\$${name}\\(([^)]+)\\)`));
  return m ? m[1] : null;
}

// --- Formatting ---

function formatAllFlags(flags: CompatFlag[]): string {
  const active = flags.filter((f) => !f.obsolete);
  const obsolete = flags.filter((f) => f.obsolete);

  const dated = active
    .filter((f) => f.enableDate || f.enableAllDates)
    .sort((a, b) => (a.enableDate || '').localeCompare(b.enableDate || ''));
  const experimental = active.filter(
    (f) => f.experimental && !f.enableDate && !f.enableAllDates
  );
  const noDate = active.filter(
    (f) => !f.experimental && !f.enableDate && !f.enableAllDates
  );

  let out = `## All Compatibility Flags (${active.length} active, ${obsolete.length} obsolete)\n\n`;

  if (dated.length > 0) {
    out += `### Date-gated (${dated.length})\n\n`;
    for (const f of dated) {
      const d = f.enableAllDates ? 'ALL DATES' : f.enableDate;
      const extra: string[] = [];
      if (f.neededByFl) extra.push('FL');
      if (f.experimental) extra.push('experimental');
      if (f.impliedBy) extra.push(`implied by ${f.impliedBy}`);
      const suffix = extra.length > 0 ? ` (${extra.join(', ')})` : '';
      out += `- **${d}**: \`${f.enableFlag || f.fieldName}\`${suffix}\n`;
    }
  }

  if (noDate.length > 0) {
    out += `\n### Opt-in only — no default date (${noDate.length})\n\n`;
    for (const f of noDate) {
      const extra: string[] = [];
      if (f.impliedBy) extra.push(`implied by ${f.impliedBy}`);
      const suffix = extra.length > 0 ? ` (${extra.join(', ')})` : '';
      out += `- \`${f.enableFlag || f.fieldName}\`${suffix}\n`;
    }
  }

  if (experimental.length > 0) {
    out += `\n### Experimental — requires --experimental (${experimental.length})\n\n`;
    for (const f of experimental) {
      out += `- \`${f.enableFlag || f.fieldName}\`\n`;
    }
  }

  return out;
}

function formatAtDate(flags: CompatFlag[], date: string): string {
  const active = flags.filter((f) => !f.obsolete);

  const enabled: CompatFlag[] = [];
  const notYetEnabled: CompatFlag[] = [];
  const optInOnly: CompatFlag[] = [];
  const experimental: CompatFlag[] = [];

  for (const f of active) {
    if (f.enableAllDates) {
      enabled.push(f);
    } else if (f.enableDate) {
      if (f.enableDate <= date) {
        enabled.push(f);
      } else {
        notYetEnabled.push(f);
      }
    } else if (f.experimental) {
      experimental.push(f);
    } else {
      optInOnly.push(f);
    }
  }

  let out = `## Flags at compatibility date "${date}"\n\n`;
  out += `- **${enabled.length}** enabled by default\n`;
  out += `- **${notYetEnabled.length}** not yet enabled (future date)\n`;
  out += `- **${optInOnly.length}** opt-in only (no date set)\n`;
  out += `- **${experimental.length}** experimental\n\n`;

  if (enabled.length > 0) {
    out += `### Enabled by default (${enabled.length})\n\n`;
    for (const f of enabled.sort((a, b) =>
      (a.enableDate || '').localeCompare(b.enableDate || '')
    )) {
      const d = f.enableAllDates ? 'all' : f.enableDate;
      out += `- \`${f.enableFlag || f.fieldName}\` (since ${d})\n`;
    }
  }

  if (notYetEnabled.length > 0) {
    out += `\n### Not yet enabled — date is after "${date}" (${notYetEnabled.length})\n\n`;
    for (const f of notYetEnabled.sort((a, b) =>
      (a.enableDate || '').localeCompare(b.enableDate || '')
    )) {
      out += `- \`${f.enableFlag || f.fieldName}\` (${f.enableDate})\n`;
    }
  }

  if (optInOnly.length > 0) {
    out += `\n### Opt-in only (${optInOnly.length})\n\n`;
    for (const f of optInOnly) {
      out += `- \`${f.enableFlag || f.fieldName}\`\n`;
    }
  }

  if (experimental.length > 0) {
    out += `\n### Experimental (${experimental.length})\n\n`;
    for (const f of experimental) {
      out += `- \`${f.enableFlag || f.fieldName}\`\n`;
    }
  }

  return out;
}
