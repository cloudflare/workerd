// Copyright (c) 2022-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * Artifacts — Git-compatible file storage on Cloudflare Workers.
 *
 * Provides programmatic access to create, manage, and fork repositories,
 * and to issue and revoke scoped access tokens.
 */

/** Information about a repository. */
interface ArtifactsRepoInfo {
  /** Unique repository ID. */
  id: string;
  /** Repository name. */
  name: string;
  /** Repository description, or null if not set. */
  description: string | null;
  /** Default branch name (e.g. "main"). */
  defaultBranch: string;
  /** ISO 8601 creation timestamp. */
  createdAt: string;
  /** ISO 8601 last-updated timestamp. */
  updatedAt: string;
  /** ISO 8601 timestamp of the last push, or null if never pushed. */
  lastPushAt: string | null;
  /** Fork source (e.g. "github:owner/repo", "artifacts:namespace/repo"), or null if not a fork. */
  source: string | null;
  /** Whether the repository is read-only. */
  readOnly: boolean;
  /** HTTPS git remote URL. */
  remote: string;
}

/** Result of creating a repository — includes the initial access token. */
interface ArtifactsCreateRepoResult {
  /** Unique repository ID. */
  id: string;
  /** Repository name. */
  name: string;
  /** Repository description, or null if not set. */
  description: string | null;
  /** Default branch name. */
  defaultBranch: string;
  /** HTTPS git remote URL. */
  remote: string;
  /** Plaintext access token (only returned at creation time). */
  token: string;
  /** ISO 8601 token expiry timestamp. */
  tokenExpiresAt: string;
}

/** Paginated list of repositories. */
interface ArtifactsRepoListResult {
  /** Repositories in this page (without the `remote` field). */
  repos: Omit<ArtifactsRepoInfo, 'remote'>[];
  /** Total number of repositories in the namespace. */
  total: number;
  /** Cursor for the next page, if there are more results. */
  cursor?: string;
}

/** Result of creating an access token. */
interface ArtifactsCreateTokenResult {
  /** Unique token ID. */
  id: string;
  /** Plaintext token (only returned at creation time). */
  plaintext: string;
  /** Token scope: "read" or "write". */
  scope: 'read' | 'write';
  /** ISO 8601 token expiry timestamp. */
  expiresAt: string;
}

/** Token metadata (no plaintext). */
interface ArtifactsTokenInfo {
  /** Unique token ID. */
  id: string;
  /** Token scope: "read" or "write". */
  scope: 'read' | 'write';
  /** Token state: "active", "expired", or "revoked". */
  state: 'active' | 'expired' | 'revoked';
  /** ISO 8601 creation timestamp. */
  createdAt: string;
  /** ISO 8601 expiry timestamp. */
  expiresAt: string;
}

/** Paginated list of tokens for a repository. */
interface ArtifactsTokenListResult {
  /** Tokens in this page. */
  tokens: ArtifactsTokenInfo[];
  /** Total number of tokens for the repository. */
  total: number;
}

/**
 * Handle for a single repository. Returned by Artifacts.get().
 *
 * Methods may throw `ArtifactsError` with code `INTERNAL_ERROR` if an unexpected service error occurs.
 */
interface ArtifactsRepo extends ArtifactsRepoInfo {
  /**
   * Create an access token for this repo.
   * @param scope Token scope: "write" (default) or "read".
   * @param ttl Time-to-live in seconds (default 86400, min 60, max 31536000).
   * @throws {ArtifactsError} with code `INVALID_TTL` if ttl is out of range.
   */
  createToken(
    scope?: 'write' | 'read',
    ttl?: number
  ): Promise<ArtifactsCreateTokenResult>;

  /** List tokens for this repo (metadata only, no plaintext). */
  listTokens(): Promise<ArtifactsTokenListResult>;

  /**
   * Revoke a token by plaintext or ID.
   * @param tokenOrId Plaintext token or token ID.
   * @returns true if revoked, false if not found.
   * @throws {ArtifactsError} with code `INVALID_INPUT` if tokenOrId is empty.
   */
  revokeToken(tokenOrId: string): Promise<boolean>;

  // ── Fork ──

  /**
   * Fork this repo to a new repo.
   * @param name Target repository name.
   * @param opts Optional: description, readOnly flag, defaultBranchOnly (default true).
   * @throws {ArtifactsError} with code `INVALID_REPO_NAME` if name is invalid.
   * @throws {ArtifactsError} with code `ALREADY_EXISTS` if the target repo already exists.
   * @throws {ArtifactsError} with code `FORK_IN_PROGRESS` if a fork is already running.
   */
  fork(
    name: string,
    opts?: {
      description?: string;
      readOnly?: boolean;
      defaultBranchOnly?: boolean;
    }
  ): Promise<ArtifactsCreateRepoResult>;
}

// ── Error types ──────────────────────────────────────────────────────────────

/**
 * Error codes returned by Artifacts binding operations.
 *
 * Each code maps to a numeric code available on `ArtifactsError.numericCode`.
 */
type ArtifactsErrorCode =
  | 'ALREADY_EXISTS'
  | 'NOT_FOUND'
  | 'IMPORT_IN_PROGRESS'
  | 'FORK_IN_PROGRESS'
  | 'INVALID_INPUT'
  | 'INVALID_REPO_NAME'
  | 'INVALID_TTL'
  | 'INVALID_URL'
  | 'REMOTE_AUTH_REQUIRED'
  | 'UPSTREAM_UNAVAILABLE'
  | 'MEMORY_LIMIT'
  | 'INTERNAL_ERROR';

/**
 * Error thrown by Artifacts binding operations.
 *
 * Uses a string `.code` discriminator following the Cloudflare platform
 * convention (StreamError, ImagesError, etc.). The `.numericCode` matches
 * the REST API `errors[].code` values.
 */
interface ArtifactsError extends Error {
  readonly name: 'ArtifactsError';
  /** String error code for programmatic matching. */
  readonly code: ArtifactsErrorCode;
  /** Numeric error code matching the REST API. */
  readonly numericCode: number;
}

// ── Binding ──────────────────────────────────────────────────────────────────

/**
 * Artifacts binding — namespace-level operations.
 *
 * Methods may throw `ArtifactsError` with code `INTERNAL_ERROR` if an unexpected service error occurs.
 */
interface Artifacts {
  /**
   * Create a new repository with an initial access token.
   * @param name Repository name (alphanumeric, dots, hyphens, underscores).
   * @param opts Optional: readOnly flag, description, default branch name.
   * @returns Repo metadata with initial token.
   * @throws {ArtifactsError} with code `INVALID_REPO_NAME` if name is invalid.
   * @throws {ArtifactsError} with code `ALREADY_EXISTS` if the repo already exists.
   */
  create(
    name: string,
    opts?: { readOnly?: boolean; description?: string; setDefaultBranch?: string }
  ): Promise<ArtifactsCreateRepoResult>;

  /**
   * Get a handle to an existing repository.
   * @param name Repository name.
   * @returns Repo handle.
   * @throws {ArtifactsError} with code `NOT_FOUND` if the repo does not exist.
   * @throws {ArtifactsError} with code `IMPORT_IN_PROGRESS` if the repo is still importing.
   * @throws {ArtifactsError} with code `FORK_IN_PROGRESS` if the repo is still forking.
   */
  get(name: string): Promise<ArtifactsRepo>;

  /**
   * Import a repository from an external git remote.
   * @param params Source URL and optional branch/depth, plus target name and options.
   * @returns Repo metadata with initial token.
   * @throws {ArtifactsError} with code `INVALID_REPO_NAME` if the target name is invalid.
   * @throws {ArtifactsError} with code `INVALID_INPUT` if the source URL is not valid HTTPS.
   * @throws {ArtifactsError} with code `INVALID_URL` if the source URL does not point to a git repository.
   * @throws {ArtifactsError} with code `REMOTE_AUTH_REQUIRED` if the remote requires authentication.
   * @throws {ArtifactsError} with code `NOT_FOUND` if the remote repository does not exist.
   * @throws {ArtifactsError} with code `UPSTREAM_UNAVAILABLE` if the remote cannot be reached.
   * @throws {ArtifactsError} with code `MEMORY_LIMIT` if the import exceeds service memory limits.
   * @throws {ArtifactsError} with code `ALREADY_EXISTS` if the target repo already exists.
   */
  import(params: {
    source: {
      url: string;
      branch?: string;
      depth?: number;
    };
    target: {
      name: string;
      opts?: {
        description?: string;
        readOnly?: boolean;
      };
    };
  }): Promise<ArtifactsCreateRepoResult>;

  /**
   * List repositories with cursor-based pagination.
   * @param opts Optional: limit (1–200, default 50), cursor for next page.
   */
  list(opts?: {
    limit?: number;
    cursor?: string;
  }): Promise<ArtifactsRepoListResult>;

  /**
   * Delete a repository and all associated tokens.
   * @param name Repository name.
   * @returns true if deleted, false if not found.
   * @throws {ArtifactsError} with code `INVALID_REPO_NAME` if name is invalid.
   */
  delete(name: string): Promise<boolean>;
}
