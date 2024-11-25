export function getServers(): string[];

export type LookupOptions = {
  family: number | string;
  hints: number;
  all: boolean;
  order: string;
  verbatim: boolean;
};
export function lookup(
  hostname: string,
  options: LookupOptions,
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

export type Resolve4Options = {
  ttl: boolean;
};
export function resolve4(
  hostname: string,
  options: Resolve4Options,
  callback: unknown
): void;

export type Resolve6Options = {
  ttl: boolean;
};
export function resolve6(
  hostname: string,
  options: Resolve6Options,
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
