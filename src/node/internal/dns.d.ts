export function lookup(
  hostname: string,
  options: {
    family: number | string;
    hints: number;
    all: boolean;
    order: string;
    verbatim: boolean;
  },
  callback: unknown
): void;
export function lookupService(
  address: string,
  port: number,
  callback: unknown
): void;
export function resolve(
  hostname: string,
  rrtype: string,
  callback: unknown
): void;
export function resolve4(
  hostname: string,
  options: { ttl?: boolean },
  callback: unknown
): void;
export function resolve6(
  hostname: string,
  options: { ttl?: boolean },
  callback: unknown
): void;
export function resolveAny(hostname: string, callback: unknown): void;
export function resolveCname(hostname: string, callback: unknown): void;
export function resolveCaa(hostname: string, callback: unknown): void;
export function resolveMx(hostname: string, callback: unknown): void;
export function resolveNaptr(hostname: string, callback: unknown): void;
export function resolveNs(hostname: string, callback: unknown): void;
export function resolvePtr(hostname: string, callback: unknown): void;
export function resolveSoa(hostname: string, callback: unknown): void;
export function resolveSrv(hostname: string, callback: unknown): void;
export function resolveTxt(hostname: string, callback: unknown): void;
export function reverse(ip: string, callback: unknown): void;
export function setDefaultResultOrder(order: string): void;
export function getDefaultResultOrder(): string;
export function setServers(servers: string[]): string;
export function getServers(): string[];

export class Resolver {
  public constructor(options: { timeout?: number; tries?: number });
  public setLocalAddress(
    ipv4: string | undefined,
    ipv6: string | undefined
  ): void;
  public cancel(): void;

  public resolve(hostname: string, rrtype: string, callback: unknown): void;
  public resolve4(
    hostname: string,
    options: { ttl?: boolean },
    callback: unknown
  ): void;
  public resolve6(
    hostname: string,
    options: { ttl?: boolean },
    callback: unknown
  ): void;
  public resolveAny(hostname: string, callback: unknown): void;
  public resolveCname(hostname: string, callback: unknown): void;
  public resolveCaa(hostname: string, callback: unknown): void;
  public resolveMx(hostname: string, callback: unknown): void;
  public resolveNaptr(hostname: string, callback: unknown): void;
  public resolveNs(hostname: string, callback: unknown): void;
  public resolvePtr(hostname: string, callback: unknown): void;
  public resolveSoa(hostname: string, callback: unknown): void;
  public resolveSrv(hostname: string, callback: unknown): void;
  public resolveTxt(hostname: string, callback: unknown): void;
  public reverse(ip: string, callback: unknown): void;
  public setServers(servers: string[]): string;
  public getServers(): string[];
}
