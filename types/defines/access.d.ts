/**
 * Represents the identity of a user authenticated via Cloudflare Access.
 * This matches the result of calling /cdn-cgi/access/get-identity.
 *
 * The exact structure of the returned object depends on the identity provider
 * configuration for the Access application. The fields below represent commonly
 * available properties, but additional provider-specific fields may be present.
 */
interface CloudflareAccessIdentity extends Record<string, unknown> {
  /** The user's email address, if available from the identity provider. */
  email?: string;
  /** The user's display name. */
  name?: string;
  /** The user's unique identifier. */
  user_uuid?: string;
  /** The Cloudflare account ID. */
  account_id?: string;
  /** Login timestamp (Unix epoch seconds). */
  iat?: number;
  /** The user's IP address at authentication time. */
  ip?: string;
  /** Authentication methods used (e.g., "pwd"). */
  amr?: string[];
  /** Identity provider information. */
  idp?: { id: string; type: string };
  /** Geographic information about where the user authenticated. */
  geo?: { country: string };
  /** Group memberships from the identity provider. */
  groups?: Array<{ id: string; name: string; email?: string }>;
  /** Device posture check results, keyed by check ID. */
  devicePosture?: Record<string, unknown>;
  /** True if the user connected via Cloudflare WARP. */
  is_warp?: boolean;
  /** True if the user is authenticated via Cloudflare Gateway. */
  is_gateway?: boolean;
}
