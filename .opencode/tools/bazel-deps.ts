import { tool } from '@opencode-ai/plugin';
import path from 'path';

// Known aliases: short name → Bazel repo name
// Built from build/deps/deps.jsonc, v8.MODULE.bazel, MODULE.bazel, etc.
const ALIASES: Record<string, string> = {
  boringssl: 'ssl',
  openssl: 'ssl',
  v8: 'workerd-v8',
  icu: 'com_googlesource_chromium_icu',
  cxx: 'workerd-cxx',
  sqlite: 'sqlite3',
  capnp: 'capnp-cpp',
  capnproto: 'capnp-cpp',
  kj: 'capnp-cpp',
};

// Ecosystem qualifiers: prefix syntax to disambiguate target type.
// e.g. "rust:base64" → look up the Rust crate, not the C++ target.
type Ecosystem = 'rust' | 'cpp' | null;

const ECOSYSTEM_PREFIXES: Record<string, Ecosystem> = {
  'rust:': 'rust',
  'crate:': 'rust',
  'cpp:': 'cpp',
  'cc:': 'cpp',
};

/** Parse optional ecosystem qualifier from target string. */
function parseQualifier(target: string): {
  ecosystem: Ecosystem;
  name: string;
} {
  for (const [prefix, ecosystem] of Object.entries(ECOSYSTEM_PREFIXES)) {
    if (target.toLowerCase().startsWith(prefix)) {
      return { ecosystem, name: target.slice(prefix.length) };
    }
  }
  return { ecosystem: null, name: target };
}

export default tool({
  description:
    'Bazel dependency lookup tool. Supports two directions:\n' +
    '- "rdeps" (default): find what workerd targets depend ON a given dependency\n' +
    '- "deps": find what a given workerd target depends on\n' +
    'Resolves short names (v8, ada-url, ssl, thiserror) to Bazel labels, ' +
    'runs queries, and returns results grouped by component.\n' +
    'Supports ecosystem qualifiers to disambiguate targets: ' +
    '"rust:base64" or "crate:base64" for Rust crates, ' +
    '"cpp:base64" or "cc:base64" for C++ targets.',
  args: {
    target: tool.schema
      .string()
      .describe(
        'Target to query. Can be a short dependency name (v8, ada-url, ssl, thiserror), ' +
          'an external label (@ada-url//:ada), an internal label (//src/workerd/jsg:jsg), ' +
          'or a file path (src/workerd/api/http.c++). ' +
          'Use ecosystem qualifiers to disambiguate: "rust:base64" or "crate:base64" ' +
          'for Rust crates, "cpp:base64" or "cc:base64" for C++ targets.'
      ),
    direction: tool.schema
      .enum(['rdeps', 'deps'])
      .optional()
      .describe(
        'Query direction: "rdeps" (default) finds what depends on this target; ' +
          '"deps" finds what this target depends on'
      ),
    depth: tool.schema
      .number()
      .optional()
      .describe(
        'Depth of search (default: 1 for direct only). ' +
          'Higher values include transitive results but are slower.'
      ),
  },
  async execute(args, ctx) {
    const root = ctx.worktree || ctx.directory;
    const depth = args.depth ?? 1;
    const direction = args.direction ?? 'rdeps';
    const { ecosystem, name } = parseQualifier(args.target.trim());

    if (direction === 'deps') {
      return await queryForwardDeps(name, depth, root, ecosystem);
    } else {
      return await queryReverseDeps(name, depth, root, ecosystem);
    }
  },
});

// =============================================================================
// Forward dependencies (deps): what does this target depend on?
// =============================================================================

async function queryForwardDeps(
  target: string,
  depth: number,
  root: string,
  ecosystem: Ecosystem = null
): Promise<string> {
  // Resolve file paths to Bazel labels
  const label = await resolveToInternalLabel(target, root, ecosystem);
  if (!label) {
    return `Could not resolve "${target}" to a Bazel target. Provide a full label like \`//src/workerd/api:http\`.`;
  }

  // Run forward deps and rdeps in parallel
  const [fwdResult, revResult] = await Promise.all([
    bazelQuery(`deps(${label}, ${depth})`, root),
    bazelQuery(`rdeps(//src/..., ${label}, 1)`, root),
  ]);

  let out = `## Dependencies: ${target}\n\n`;
  out += `**Target:** \`${label}\`\n\n`;

  // Forward deps: split into internal and external
  const fwdTargets = fwdResult
    .filter((t) => t !== label)
    .filter(
      (t) =>
        !t.startsWith('@bazel_tools') &&
        !t.startsWith('@platforms') &&
        !t.startsWith('@@rules_')
    );

  const internal = fwdTargets.filter((t) => t.startsWith('//src/')).sort();
  const external = fwdTargets
    .filter((t) => t.startsWith('@') || t.startsWith('@@'))
    .sort();

  // Group internal deps
  if (internal.length > 0) {
    const groups = groupByComponent(internal);
    out += `### Direct dependencies — internal (${internal.length} targets)\n\n`;
    for (const [component, targets] of Object.entries(groups).sort()) {
      out += `**${component}/**\n`;
      for (const t of targets.slice(0, 15)) {
        out += `- \`${t}\`\n`;
      }
      if (targets.length > 15) {
        out += `- ... and ${targets.length - 15} more\n`;
      }
      out += '\n';
    }
  }

  // Group external deps by repo
  if (external.length > 0) {
    const byRepo = groupByRepo(external);
    out += `### Direct dependencies — external (${external.length} targets across ${Object.keys(byRepo).length} repos)\n\n`;
    for (const [repo, targets] of Object.entries(byRepo).sort()) {
      if (targets.length <= 3) {
        out += `- **${repo}**: ${targets.map((t) => `\`${t}\``).join(', ')}\n`;
      } else {
        out += `- **${repo}**: ${targets.length} targets\n`;
      }
    }
    out += '\n';
  }

  // Reverse deps (what depends on this target)
  const revTargets = revResult
    .filter((t) => t !== label && t.startsWith('//src/'))
    .sort();

  if (revTargets.length > 0) {
    const groups = groupByComponent(revTargets);
    out += `### Reverse dependencies (${revTargets.length} targets depend on this)\n\n`;
    for (const [component, targets] of Object.entries(groups).sort()) {
      out += `**${component}/**\n`;
      for (const t of targets.slice(0, 15)) {
        out += `- \`${t}\`\n`;
      }
      if (targets.length > 15) {
        out += `- ... and ${targets.length - 15} more\n`;
      }
      out += '\n';
    }
  } else {
    out += `### Reverse dependencies\n\nNo targets within \`//src/...\` directly depend on this target.\n\n`;
  }

  return out;
}

// =============================================================================
// Reverse dependencies (rdeps): what depends on this target?
// =============================================================================

async function queryReverseDeps(
  dep: string,
  depth: number,
  root: string,
  ecosystem: Ecosystem = null
): Promise<string> {
  // Resolve the dependency to Bazel label(s)
  const resolved = await resolveDep(dep, root, ecosystem);
  if (resolved.error) {
    return resolved.error;
  }

  let out = `## Reverse dependencies: ${dep}\n\n`;
  out += `**Resolved labels:** ${resolved.labels.map((l) => `\`${l}\``).join(', ')}\n`;
  if (resolved.note) {
    out += `**Note:** ${resolved.note}\n`;
  }
  out += '\n';

  // Run rdeps queries for all labels in parallel
  const allResults = await Promise.all(
    resolved.labels.map((label) =>
      bazelQuery(`rdeps(//src/..., ${label}, ${depth})`, root)
    )
  );

  // Merge and deduplicate
  const allTargets = new Set<string>();
  for (const result of allResults) {
    for (const target of result) {
      allTargets.add(target);
    }
  }

  const srcTargets = [...allTargets]
    .filter((t) => t.startsWith('//src/'))
    .sort();

  if (srcTargets.length === 0) {
    out += `No direct dependents found within \`//src/...\`.\n\n`;
    out +=
      'This dependency may only be consumed transitively through another dependency, ';
    out += 'or the label may not be correct. Check with:\n';
    out += '```\n';
    for (const label of resolved.labels) {
      const repo = label.split('//')[0];
      out += `bazel query '${repo}//...' --output label 2>/dev/null | head -10\n`;
    }
    out += '```\n';
    return out;
  }

  const groups = groupByComponent(srcTargets);

  out += `### Direct dependents (${srcTargets.length} targets)\n\n`;
  for (const [component, targets] of Object.entries(groups).sort()) {
    out += `**${component}/**\n`;
    for (const t of targets.slice(0, 15)) {
      out += `- \`${t}\`\n`;
    }
    if (targets.length > 15) {
      out += `- ... and ${targets.length - 15} more\n`;
    }
    out += '\n';
  }

  // Summary
  out += `### Summary\n\n`;
  const componentCounts = Object.entries(groups)
    .map(([c, ts]) => `${c} (${ts.length})`)
    .join(', ');
  out += `**${srcTargets.length} targets** across ${Object.keys(groups).length} components: ${componentCounts}\n`;

  if (Object.keys(groups).length === 1) {
    out += `\n**Narrow usage** — only consumed by the \`${Object.keys(groups)[0]}/\` component.\n`;
  } else if (Object.keys(groups).length <= 3) {
    out += `\n**Moderate usage** — consumed by ${Object.keys(groups).length} components.\n`;
  } else {
    out += `\n**Broad usage** — consumed across ${Object.keys(groups).length} components.\n`;
  }

  return out;
}

// =============================================================================
// Label resolution
// =============================================================================

interface ResolveResult {
  labels: string[];
  note?: string;
  error?: string;
}

async function resolveDep(
  dep: string,
  root: string,
  ecosystem: Ecosystem = null
): Promise<ResolveResult> {
  // Already a full label
  if (dep.startsWith('//') || dep.startsWith('@')) {
    return { labels: [dep] };
  }

  // Check aliases (only for C++ or unqualified lookups)
  const canonical =
    ecosystem !== 'rust' ? (ALIASES[dep.toLowerCase()] ?? dep) : dep;

  // Rust-only lookup: skip C++ resolution entirely
  if (ecosystem === 'rust') {
    const crateLabels = await discoverRustCrate(dep, root);
    if (crateLabels.length > 0) {
      return { labels: crateLabels, note: 'Resolved as Rust crate' };
    }
    return {
      labels: [],
      error:
        `Could not resolve Rust crate "${dep}".\n\n` +
        `Tried: \`@crates_vendor//:${dep}\`\n\n` +
        `Check available crates with: \`ls deps/rust/crates/BUILD.${dep}-*.bazel\``,
    };
  }

  // C++-only lookup: skip Rust resolution entirely
  if (ecosystem === 'cpp') {
    const extLabels = await discoverExternalTargets(canonical, root);
    if (extLabels.length > 0) {
      const note =
        canonical !== dep
          ? `Resolved alias "${dep}" → "${canonical}"`
          : undefined;
      return { labels: extLabels, note };
    }
    const fallbackLabels = await discoverExternalTargets(`@${canonical}`, root);
    if (fallbackLabels.length > 0) {
      return { labels: fallbackLabels };
    }
    return {
      labels: [],
      error:
        `Could not resolve C++ dependency "${dep}".\n\n` +
        `Tried: \`@${canonical}\`\n\n` +
        `You can provide a full Bazel label instead (e.g., \`@ada-url//:ada\`).`,
    };
  }

  // Unqualified: try C++ first, then Rust (existing behavior)
  const extLabels = await discoverExternalTargets(canonical, root);
  if (extLabels.length > 0) {
    const note =
      canonical !== dep
        ? `Resolved alias "${dep}" → "${canonical}"`
        : undefined;
    return { labels: extLabels, note };
  }

  // Try as a Rust crate via crates_vendor
  const crateLabels = await discoverRustCrate(dep, root);
  if (crateLabels.length > 0) {
    return { labels: crateLabels, note: 'Resolved as Rust crate' };
  }

  // Try with @ prefix as a last resort
  const fallbackLabels = await discoverExternalTargets(`@${canonical}`, root);
  if (fallbackLabels.length > 0) {
    return { labels: fallbackLabels };
  }

  return {
    labels: [],
    error:
      `Could not resolve "${dep}" to a Bazel dependency label.\n\n` +
      `Tried:\n` +
      `- External repo: \`@${canonical}\`\n` +
      `- Rust crate: \`@crates_vendor//:${dep}\`\n\n` +
      `You can provide a full Bazel label instead, or use a qualifier ` +
      `(e.g., \`rust:${dep}\` or \`cpp:${dep}\`) to narrow the search.`,
  };
}

// Resolve a file path or short name to an internal //src/... label
// For ecosystem-qualified targets (rust:foo, cpp:foo), resolve via resolveDep instead.
async function resolveToInternalLabel(
  target: string,
  root: string,
  ecosystem: Ecosystem = null
): Promise<string | null> {
  // Already a Bazel label
  if (target.startsWith('//') || target.startsWith('@')) {
    return target;
  }

  // Ecosystem-qualified: resolve as a dependency, not an internal target
  if (ecosystem !== null) {
    const resolved = await resolveDep(target, root, ecosystem);
    if (resolved.labels.length > 0) {
      return resolved.labels[0];
    }
    return null;
  }

  // File path — find its Bazel target
  const filePath = target.startsWith('src/') ? target : `src/${target}`;
  const dir = path.dirname(filePath);

  try {
    // Use bazel query to find the target that owns this file
    const result = await bazelQuery(`kind("rule", //${dir}/...)`, root);
    if (result.length > 0) {
      // Try to find a target matching the file's base name
      const baseName = path.basename(filePath).replace(/\.[^.]+$/, '');
      const exactMatch = result.find((t) => t.endsWith(`:${baseName}`));
      return exactMatch ?? result[0];
    }
  } catch {
    // fall through
  }

  // Last resort: try as a label directly
  return `//${dir}:${path.basename(dir)}`;
}

// =============================================================================
// External dependency discovery
// =============================================================================

async function discoverExternalTargets(
  repoName: string,
  root: string
): Promise<string[]> {
  const repo = repoName.startsWith('@') ? repoName : `@${repoName}`;
  try {
    const targets = await bazelQuery(`${repo}//...`, root);
    if (targets.length === 0) return [];

    // Prefer short top-level targets
    const topLevel = targets.filter((t) => {
      const label = t.split('//')[1] || '';
      const parts = label.split(':');
      return parts.length === 2 && !parts[1].includes('/');
    });

    const candidates = topLevel.length > 0 ? topLevel : targets;
    return candidates.slice(0, 5);
  } catch {
    return [];
  }
}

async function discoverRustCrate(
  crateName: string,
  root: string
): Promise<string[]> {
  try {
    const targets = await bazelQuery('@crates_vendor//...', root);
    const matching = targets.filter((l) => {
      const lower = l.toLowerCase();
      return (
        lower.includes(`:${crateName.toLowerCase()}`) ||
        lower.includes(`/${crateName.toLowerCase()}`)
      );
    });
    if (matching.length > 0) {
      return matching.slice(0, 3);
    }
  } catch {
    // crates_vendor may not exist
  }

  try {
    const targets = await bazelQuery(
      `filter("crates_vendor__${crateName}", //external:all-targets)`,
      root
    );
    if (targets.length > 0) {
      return targets.slice(0, 3);
    }
  } catch {
    // not found
  }

  return [];
}

// =============================================================================
// Bazel query helper
// =============================================================================

async function bazelQuery(query: string, root: string): Promise<string[]> {
  try {
    const output =
      await Bun.$`bazel query '${query}' --output label 2>/dev/null`
        .cwd(root)
        .text();
    return output
      .split('\n')
      .map((l) => l.trim())
      .filter(Boolean);
  } catch {
    return [];
  }
}

// =============================================================================
// Grouping helpers
// =============================================================================

function groupByComponent(targets: string[]): Record<string, string[]> {
  const groups: Record<string, string[]> = {};
  for (const target of targets) {
    let component = 'other';
    const m = target.match(/^\/\/src\/(.+?):/);
    if (m) {
      let p = m[1];
      if (p.startsWith('workerd/')) p = p.slice('workerd/'.length);
      component = p;
    }
    if (!groups[component]) groups[component] = [];
    groups[component].push(target);
  }
  return groups;
}

function groupByRepo(targets: string[]): Record<string, string[]> {
  const groups: Record<string, string[]> = {};
  for (const target of targets) {
    let repo: string;
    if (target.startsWith('@@')) {
      // @@+crate_repositories+crates_vendor__foo-1.0 → crates_vendor__foo-1.0
      const m = target.match(/@@[^/]*\+([^/]+)/);
      repo = m ? m[1] : target.split('//')[0];
    } else {
      // @foo//bar:baz → @foo
      repo = target.split('//')[0];
    }
    if (!groups[repo]) groups[repo] = [];
    groups[repo].push(target);
  }
  return groups;
}
