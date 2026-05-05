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

/** Handle for a single repository. Returned by Artifacts.get(). */
interface ArtifactsRepo extends ArtifactsRepoInfo {
  /**
   * Create an access token for this repo.
   * @param scope Token scope: "write" (default) or "read".
   * @param ttl Time-to-live in seconds (default 86400, min 60, max 31536000).
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
   */
  revokeToken(tokenOrId: string): Promise<boolean>;

  // ── Fork ──

  /**
   * Fork this repo to a new repo.
   * @param name Target repository name.
   * @param opts Optional: description, readOnly flag, defaultBranchOnly (default true).
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

/** Artifacts binding — namespace-level operations. */
interface Artifacts {
  /**
   * Create a new repository with an initial access token.
   * @param name Repository name (alphanumeric, dots, hyphens, underscores).
   * @param opts Optional: readOnly flag, description, default branch name.
   * @returns Repo metadata with initial token.
   */
  create(
    name: string,
    opts?: { readOnly?: boolean; description?: string; setDefaultBranch?: string }
  ): Promise<ArtifactsCreateRepoResult>;

  /**
   * Get a handle to an existing repository.
   * @param name Repository name.
   * @returns Repo handle.
   */
  get(name: string): Promise<ArtifactsRepo>;

  /**
   * Import a repository from an external git remote.
   * @param params Source URL and optional branch/depth, plus target name and options.
   * @returns Repo metadata with initial token.
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
   */
  delete(name: string): Promise<boolean>;
}
