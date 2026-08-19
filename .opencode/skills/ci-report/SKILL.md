---
name: ci-report
description: CI status report for a workerd PR. Covers GitHub Actions checks and, for Cloudflare employees, the internal GitLab pipeline. Reports pass/fail for every job, extracts error details from failures, and offers to retry flaky jobs on either platform. Load this skill when the user asks about CI status, test failures, build failures, or wants to retry a failed CI job.
---

# CI Report

Produce a CI status report for a workerd PR. The report covers GitHub Actions
and, when accessible, the internal GitLab CI pipeline.

## Inputs

The user provides a PR number or URL. If omitted, the tool detects the current
branch's open PR automatically.

## Step 1: Gather GitHub CI data

Use the **`ci-report`** custom tool to collect all GitHub-side data in a single
call:

```
ci-report({ pr: 7017 })
```

This returns a JSON object containing:
- `github.checks` — all GitHub Actions check statuses
- `github.failures` — error snippets extracted from failed job logs
- `internalBuild` — metadata parsed from the `internal-build` job log:
  - `projectPath` and `pipelineId` (from failure URL, if present)
  - `headRef` and `userLogin` (from log env vars)
  - `branchRef` (the ref passed to the internal build script)
  - `status` (pass/fail/absent)
- `changedFiles` — files modified by the PR (for triage)
- `mainBranch` — recent CI runs on `main` (for comparison)

If `internalBuild.pipelineUrl` is present, use `projectPath` and `pipelineId`
directly for GitLab queries. If it is null (the URL only appears on failure),
use the `headRef` to search for the pipeline via `list_pipelines`, trying both
the direct branch name and the `workerd-robot/` prefixed variant.

## Step 2: Internal GitLab Pipeline

Gather all GitLab data in a **single** `portal_codemode_execute` call. This
replaces 4-6 separate MCP tool calls with one.

If the project path is not known from Step 1, determine it from parent project
context: check `../../.git/config` or `../../.gitlab-ci.yml`, or load the
`parent-project-skills` skill. If it cannot be determined, skip this step.

```javascript
// portal_codemode_execute — single call for all GitLab CI data
async () => {
  const projectId = "<project-path>";

  // If we have a pipeline ID from Step 1, use it directly.
  // Otherwise, find the pipeline by branch ref.
  let pipelineId = <ID or null>;

  if (!pipelineId) {
    // Try workerd-robot/ prefix first, then direct branch name
    for (const ref of ["workerd-robot/<headRef>", "<headRef>"]) {
      try {
        const res = await codemode.gitlab_mcp_server_list_pipelines({
          query: { project_id: projectId, ref, per_page: 3 }
        });
        const pipelines = JSON.parse(res[0].text);
        if (pipelines.length > 0) {
          pipelineId = pipelines[0].id;
          break;
        }
      } catch { continue; }
    }
    if (!pipelineId) return { error: "No pipeline found" };
  }

  // Fetch failed and successful jobs in parallel
  const [failedRes, successRes] = await Promise.allSettled([
    codemode.gitlab_mcp_server_list_pipeline_jobs({
      query: { project_id: projectId, pipeline_id: pipelineId,
               scope: "failed", per_page: 50 }
    }),
    codemode.gitlab_mcp_server_list_pipeline_jobs({
      query: { project_id: projectId, pipeline_id: pipelineId,
               scope: "success", per_page: 50 }
    }),
  ]);

  const failedJobs = failedRes.status === "fulfilled"
    ? JSON.parse(failedRes.value[0].text) : [];
  const successJobs = successRes.status === "fulfilled"
    ? JSON.parse(successRes.value[0].text) : [];

  // Fetch logs for failed jobs in parallel, extract errors
  const failures = await Promise.all(failedJobs.map(async (j) => {
    try {
      const log = await codemode.gitlab_mcp_server_get_job_log({
        query: { project_id: projectId, job_id: j.id }
      });
      const logText = JSON.parse(log[0].text).log;
      const errors = [];
      const lines = logText.split("\n");
      for (let i = 0; i < lines.length; i++) {
        if (/error:|error\[|ERROR:|FAILED|FAIL:|fatal:|make: \*\*\*/.test(lines[i])) {
          const snippet = lines.slice(Math.max(0,i-2), Math.min(lines.length,i+6)).join("\n");
          if (errors.length < 5) errors.push(snippet);
        }
      }
      return { id: j.id, name: j.name, status: j.status,
               allow_failure: j.allow_failure,
               duration: Math.round(j.duration)+"s", errors };
    } catch {
      return { id: j.id, name: j.name, status: j.status,
               allow_failure: j.allow_failure,
               duration: Math.round(j.duration)+"s",
               errors: ["Failed to fetch log"] };
    }
  }));

  return {
    pipelineId,
    passed: successJobs.map(j => ({
      name: j.name, duration: Math.round(j.duration)+"s"
    })),
    failures,
  };
}
```

If the call fails with an authentication or authorization error, report:

> GitLab CI: not accessible (you may not have the GitLab MCP server enabled
> or lack project access). GitHub-only report shown above.

## Step 3: Failure Triage

For every failure found in Steps 1-2, determine whether it is **material**
(caused by this PR's changes) or **likely a flake / pre-existing breakage**.
This is the most important part of the report.

### 3a. Files changed

Use `changedFiles` from the Step 1 tool output — no additional call needed.

### 3b. Classify each failure

For each failure, apply these checks in order:

**Material (caused by this PR):**
- The error references a file, symbol, type, or crate that the PR modifies
- The error is a compile error in code the PR touches or in code that directly
  depends on code the PR touches (e.g., a type change that breaks a downstream
  consumer)
- The error is a test failure in a test the PR modifies, or a test that
  exercises functionality the PR changes
- The error mentions a symbol the PR added, removed, renamed, or changed the
  signature of

**Likely flake / pre-existing:**
- The error is an infrastructure issue: runner failures, DNS resolution,
  network timeouts, Docker pull failures, disk space, OOM kills
- The error is in code completely unrelated to the PR's changes (different
  crate, different directory, no dependency relationship)
- The same failure appears on the `main` branch or other recent PRs (check
  via `gh run list --repo cloudflare/workerd --branch main --limit 5` if
  uncertain)
- The error is a test timeout with no assertion failure
- The error references `sudo: unable to resolve host` or similar CI
  environment issues

**Uncertain:**
- If the relationship is unclear, say so and explain what would need to be
  checked. Do not guess.

### 3c. Check main branch for comparison (when helpful)

Use `mainBranch` from the Step 1 tool output to see if `main` is also failing.
If a specific workflow is failing on `main` with the same conclusion, the
failure is pre-existing and not caused by this PR.

## Step 4: Combined Report

Present a single combined report with two sections:

### GitHub Actions
- Total: N jobs, N passed, N failed, N skipped/pending
- Table of all jobs
- Error details for failures (collapsed if many)

### Internal GitLab CI
- Pipeline URL (the full URL extracted from `internal-build` logs)
- Total: N jobs, N passed, N failed
- Table of all jobs
- Error details for failures

### Failure Classification

For each failure, state the classification clearly:

| Failure | Classification | Reasoning |
| ------- | -------------- | --------- |

### Verdict

State one of:
- **All green** — all jobs on both platforms pass
- **GitHub green, GitLab failing** — with root cause summary
- **GitHub failing** — with root cause summary
- **Both failing** — with root cause summary
- **GitHub green, GitLab unknown** — if GitLab is inaccessible

Qualify the verdict with the triage results. For example: "GitLab failing, but
all failures are material — the PR removes `Send` from `KjOwn` and an internal
consumer relies on it" or "GitLab failing, but the failure is pre-existing on
main and unrelated to this PR."

If the same error appears across multiple GitLab jobs (common for compile
errors), deduplicate: show the error once and list which jobs hit it.

## Step 5: Retry Offer

If any jobs failed, offer to retry based on the triage from Step 3.

### GitHub retry capabilities

Retry individual failed runs or all failed jobs:

```bash
# Rerun only failed jobs in a run
gh run rerun <run-id> --failed --repo cloudflare/workerd

# Rerun a specific job (use databaseId, NOT the job number from the URL)
# First get the correct ID:
gh run view <run-id> --json jobs --jq '.jobs[] | {name, databaseId}'
# Then:
gh run rerun <run-id> --job <databaseId> --repo cloudflare/workerd
```

### GitLab retry capabilities

Retry individual jobs or the entire pipeline (using the project path and IDs
extracted from the `internal-build` log):

```javascript
// Retry a single job
gitlab_mcp_server_retry_job({
  query: {
    project_id: "<project-path>",
    job_id: <job_id>
  }
})

// Retry all failed jobs in a pipeline
gitlab_mcp_server_retry_pipeline({
  query: {
    project_id: "<project-path>",
    pipeline_id: <pipeline_id>
  }
})
```

### When to suggest retries

Use the classification from Step 3 to guide the recommendation:

- **Flaky/infra failures**: suggest retry. These are timeouts, network errors,
  runner issues, DNS resolution failures, Docker pull failures, etc.
- **Pre-existing failures**: suggest retry only if there is reason to believe
  the failure is intermittent. If it fails consistently on `main`, a retry
  will not help — say so.
- **Material failures**: do NOT suggest retry — these are real failures caused
  by the PR that need code fixes. Explain what needs to change.
- **Mixed**: separate the flaky from the material and offer to retry only the
  flaky ones.

Always confirm with the user before executing any retry.

## Notes

- The `internal-build` GitHub job triggers GitLab via
  `tools/cross/internal_build.py`. The GitLab branch name depends on whether
  a matching branch already exists in the internal project: if it does, that
  branch is used directly; otherwise, the branch is prefixed with
  `workerd-robot/`. The actual branch used is visible in the `internal-build`
  job log and in the GitLab pipeline's `ref` field.
- Internal patches are applied on top of the workerd checkout in GitLab
  (visible in the log as "Applying ... to deps/workerd..."). Failures caused
  by patch conflicts are distinct from code errors — flag them as such.
- GitLab job names typically follow patterns like `x64-release-build`,
  `x64-debug-build`, `x64-asan-debug-build`, `x64-ubsan-build`,
  `arm64-build`, `lint-build`. The lint job runs clippy and formatting checks
  and is typically the fastest to finish.
