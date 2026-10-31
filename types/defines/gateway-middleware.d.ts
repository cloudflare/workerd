// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/** Gateway-managed continuation to the original upstream. */
type GatewayMiddlewareNext = (request: Request) => Promise<Response>;

/** Gateway metadata describing the original request. */
interface GatewayMiddlewareContext {
  readonly srcIp: string;
  readonly srcPort: number;
  readonly dstIp?: string;
  readonly dstPort?: number;

  readonly networkProtocol: string;
  readonly embeddedProtocols?: readonly string[];
  readonly sni?: string;

  readonly host: string;

  /** Original request URL normalized by Gateway. */
  readonly uri: string;

  /** Destination category IDs, not category names. */
  readonly domainCategories?: readonly number[];

  readonly onrampType?: string;
  readonly vnetId?: string;
  readonly proxyEndpoint?: string;
  readonly internalSrcIp?: string;

  readonly accountId: string;

  /** UUID of the policy that selected this middleware. */
  readonly policyId: string;

  /** Rejects if identity is unavailable. */
  identity(): Promise<GatewayMiddlewareIdentity>;
  /** Rejects if device posture is unavailable. */
  devicePosture(): Promise<GatewayMiddlewareDevicePosture>;
}

interface GatewayMiddlewareIdentity {
  readonly email?: string;
  readonly name?: string;
  readonly groups?: readonly GatewayMiddlewareIdentityGroup[];
  readonly samlAttributes?: readonly string[];
  readonly oidcClaims?: readonly string[];
}

interface GatewayMiddlewareIdentityGroup {
  readonly id?: string;
  readonly email?: string;
  readonly name?: string;
}

interface GatewayMiddlewareDevicePosture {
  readonly checks: GatewayMiddlewareDevicePostureChecks;
}

interface GatewayMiddlewareDevicePostureChecks {
  /** Identifiers of posture checks that passed; may be empty. */
  readonly passed: readonly string[];
}

/**
 * The contract of a Gateway middleware entrypoint. Implement it on a `WorkerEntrypoint` and
 * select that export in a Gateway policy. A Worker may export any number of middleware
 * entrypoints.
 *
 * @example
 * ```ts
 * import { WorkerEntrypoint } from "cloudflare:workers";
 *
 * export class RequireIdentity extends WorkerEntrypoint implements GatewayMiddleware {
 *   async handle(
 *     request: Request,
 *     context: GatewayMiddlewareContext,
 *     next: GatewayMiddlewareNext,
 *   ): Promise<Response> {
 *     try {
 *       await context.identity();
 *     } catch {
 *       return new Response("Forbidden", { status: 403 });
 *     }
 *     return next(request);
 *   }
 * }
 * ```
 */
interface GatewayMiddleware {
  handle(
    request: Request,
    context: GatewayMiddlewareContext,
    next: GatewayMiddlewareNext
  ): Promise<Response>;
}
