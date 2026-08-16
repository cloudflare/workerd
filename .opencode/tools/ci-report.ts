import { tool } from '@opencode-ai/plugin';

export default tool({
  description:
    'Gather CI status for a workerd GitHub PR. Returns a structured report of all ' +
    'GitHub Actions checks, error details from failed jobs, internal GitLab pipeline ' +
    'metadata (project path, pipeline ID, branch ref) extracted from the internal-build ' +
    'job log, the list of files changed by the PR, and recent main branch CI status ' +
    'for comparison. This single call replaces 6-8 individual gh/grep/tail calls.',
  args: {
    pr: tool.schema
      .number()
      .describe('PR number. If omitted, detects from the current branch.')
      .optional(),
  },
  async execute(args, ctx) {
    const repo = 'cloudflare/workerd';

    // Resolve PR number
    let pr = args.pr;
    if (!pr) {
      try {
        const result =
          await Bun.$`gh pr view --repo ${repo} --json number --jq .number`
            .text();
        pr = parseInt(result.trim(), 10);
        if (isNaN(pr)) return 'No open PR found for the current branch.';
      } catch {
        return 'No open PR found for the current branch. Provide a PR number.';
      }
    }

    // Run independent queries in parallel
    const [checksRaw, diffRaw, mainRunsRaw] = await Promise.all([
      Bun.$`gh pr checks ${pr} --repo ${repo}`.text().catch((e: any) => e.stdout?.toString() ?? ''),
      Bun.$`gh pr diff ${pr} --repo ${repo} --name-only`.text().catch(() => ''),
      Bun.$`gh run list --repo ${repo} --branch main --limit 5 --json databaseId,conclusion,displayTitle,event,headBranch,workflowName`
        .text()
        .catch(() => '[]'),
    ]);

    // Parse checks
    const checks = parseChecks(checksRaw);
    const failedChecks = checks.filter((c) => c.status === 'fail');

    // Extract error details from failed jobs (parallel)
    const failureDetails = await Promise.all(
      failedChecks.map(async (check) => {
        const jobId = extractJobId(check.url);
        if (!jobId) return { ...check, errors: [], log: '' };
        try {
          const log =
            await Bun.$`gh api repos/${repo}/actions/jobs/${jobId}/logs`
              .text();
          const errors = extractErrors(log);
          return { ...check, errors, log };
        } catch {
          return { ...check, errors: ['Failed to fetch job log'], log: '' };
        }
      })
    );

    // Extract internal-build info
    const internalBuild = extractInternalBuildInfo(failureDetails, checks);

    // If internal-build passed but we didn't get a pipeline URL from failures,
    // fetch its log separately to get the branch ref
    if (!internalBuild.pipelineUrl && internalBuild.jobId) {
      try {
        const log =
          await Bun.$`gh api repos/${repo}/actions/jobs/${internalBuild.jobId}/logs`
            .text();
        const info = parseInternalBuildLog(log);
        Object.assign(internalBuild, info);
      } catch {
        // Not critical
      }
    }

    // Parse changed files
    const changedFiles = diffRaw
      .trim()
      .split('\n')
      .filter((f) => f.length > 0);

    // Parse main branch runs
    let mainRuns: any[] = [];
    try {
      mainRuns = JSON.parse(mainRunsRaw);
    } catch {
      // ignore
    }

    // Build the report
    const report: CIReport = {
      pr,
      github: {
        total: checks.length,
        passed: checks.filter((c) => c.status === 'pass').length,
        failed: failedChecks.length,
        skipped: checks.filter(
          (c) => c.status !== 'pass' && c.status !== 'fail'
        ).length,
        checks,
        failures: failureDetails.filter((f) => f.errors.length > 0),
      },
      internalBuild,
      changedFiles,
      mainBranch: mainRuns.map((r: any) => ({
        id: r.databaseId,
        conclusion: r.conclusion,
        workflow: r.workflowName,
        title: r.displayTitle,
      })),
    };

    return JSON.stringify(report, null, 2);
  },
});

// --- Types ---

interface Check {
  name: string;
  status: string;
  duration: string;
  url: string;
}

interface CIReport {
  pr: number;
  github: {
    total: number;
    passed: number;
    failed: number;
    skipped: number;
    checks: Check[];
    failures: Array<Check & { errors: string[]; log: string }>;
  };
  internalBuild: InternalBuildInfo;
  changedFiles: string[];
  mainBranch: Array<{
    id: number;
    conclusion: string;
    workflow: string;
    title: string;
  }>;
}

interface InternalBuildInfo {
  status: string;
  jobId: string | null;
  runId: string | null;
  pipelineUrl: string | null;
  pipelineId: string | null;
  projectPath: string | null;
  branchRef: string | null;
  headRef: string | null;
  userLogin: string | null;
}

// --- Parsing helpers ---

function parseChecks(raw: string): Check[] {
  return raw
    .trim()
    .split('\n')
    .filter((line) => line.length > 0)
    .map((line) => {
      const parts = line.split('\t');
      return {
        name: parts[0]?.trim() ?? '',
        status: parts[1]?.trim() ?? '',
        duration: parts[2]?.trim() ?? '',
        url: parts[3]?.trim() ?? '',
      };
    })
    .filter((c) => c.name.length > 0);
}

function extractJobId(url: string): string | null {
  // URL format: .../actions/runs/<run-id>/job/<job-id>
  const m = url.match(/\/job(?:s)?\/(\d+)/);
  return m ? m[1] : null;
}

function extractErrors(log: string): string[] {
  const lines = log.split('\n');
  const errors: string[] = [];
  const seen = new Set<string>();

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (
      /\b(ERROR:|FAILED:|error\[|error:|fatal:|make: \*\*\*|Process completed with exit code [^0])/.test(
        line
      )
    ) {
      // Grab context: 2 lines before, 5 after
      const start = Math.max(0, i - 2);
      const end = Math.min(lines.length - 1, i + 5);
      const snippet = lines
        .slice(start, end + 1)
        .map((l) => l.replace(/^\d{4}-\d{2}-\d{2}T[\d:.]+Z\s*\d+[OE]\s*/, ''))
        .join('\n');

      // Deduplicate identical snippets
      const key = snippet.slice(0, 200);
      if (!seen.has(key)) {
        seen.add(key);
        errors.push(snippet);
      }
    }
  }

  // Cap at 10 error snippets to avoid huge output
  return errors.slice(0, 10);
}

function extractInternalBuildInfo(
  failureDetails: Array<Check & { errors: string[]; log: string }>,
  allChecks: Check[]
): InternalBuildInfo {
  const info: InternalBuildInfo = {
    status: 'unknown',
    jobId: null,
    runId: null,
    pipelineUrl: null,
    pipelineId: null,
    projectPath: null,
    branchRef: null,
    headRef: null,
    userLogin: null,
  };

  // Find internal-build check
  const ibCheck = allChecks.find((c) => c.name === 'internal-build');
  if (!ibCheck) {
    info.status = 'absent';
    return info;
  }

  info.status = ibCheck.status;
  info.jobId = extractJobId(ibCheck.url);

  const runMatch = ibCheck.url.match(/\/runs\/(\d+)/);
  info.runId = runMatch ? runMatch[1] : null;

  // If it failed, we already have the log in failureDetails
  const ibFailure = failureDetails.find((f) => f.name === 'internal-build');
  if (ibFailure?.log) {
    Object.assign(info, parseInternalBuildLog(ibFailure.log));
  }

  return info;
}

function parseInternalBuildLog(log: string): Partial<InternalBuildInfo> {
  const result: Partial<InternalBuildInfo> = {};

  // Extract pipeline URL (only present on failure)
  const pipelineMatch = log.match(
    /Internal pipeline (?:failed|succeeded):\s*(https:\/\/gitlab\.cfdata\.org\/([^/]+(?:\/[^/]+)*)\/\-\/pipelines\/(\d+))/
  );
  if (pipelineMatch) {
    result.pipelineUrl = pipelineMatch[1];
    result.projectPath = pipelineMatch[2];
    result.pipelineId = pipelineMatch[3];
  }

  // Extract branch ref
  const headRefMatch = log.match(/HEAD_REF:\s*(\S+)/);
  if (headRefMatch) result.headRef = headRefMatch[1];

  const userMatch = log.match(/USER_LOGIN:\s*(\S+)/);
  if (userMatch) result.userLogin = userMatch[1];

  // Extract the actual ref used (could be direct or workerd-robot/ prefixed)
  const refMatch = log.match(/REF="([^"]+)"/g);
  if (refMatch) {
    // Last REF= assignment is the one that's used (the else branch for non-forks)
    const last = refMatch[refMatch.length - 1];
    const m = last.match(/REF="([^"]+)"/);
    if (m) result.branchRef = m[1];
  }

  return result;
}
