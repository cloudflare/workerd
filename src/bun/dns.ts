// bun:dns - DNS via DNS-over-HTTPS (Cloudflare/Google)
type RecordType =
  | 'A'
  | 'AAAA'
  | 'CNAME'
  | 'MX'
  | 'TXT'
  | 'NS'
  | 'SOA'
  | 'SRV'
  | 'PTR'

// DNS response types
interface DNSAnswer {
  name: string
  type: number
  TTL: number
  data: string
}

interface DNSResponse {
  Status: number
  TC: boolean
  RD: boolean
  RA: boolean
  AD: boolean
  CD: boolean
  Question: Array<{ name: string; type: number }>
  Answer?: DNSAnswer[]
  Authority?: DNSAnswer[]
}

// Record type number mapping
const RECORD_TYPES: Record<RecordType, number> = {
  A: 1,
  AAAA: 28,
  CNAME: 5,
  MX: 15,
  TXT: 16,
  NS: 2,
  SOA: 6,
  SRV: 33,
  PTR: 12,
}

// DoH providers
const DOH_PROVIDERS = {
  cloudflare: 'https://cloudflare-dns.com/dns-query',
  google: 'https://dns.google/resolve',
} as const

type DoHProvider = keyof typeof DOH_PROVIDERS

// Configuration
let currentProvider: DoHProvider = 'cloudflare'
let customDoHEndpoint: string | null = null

export function setProvider(provider: DoHProvider): void {
  if (!DOH_PROVIDERS[provider]) {
    throw new Error(
      `Unknown DNS provider: ${provider}. Use 'cloudflare' or 'google'.`,
    )
  }
  currentProvider = provider
}

export function getProvider(): DoHProvider {
  return currentProvider
}

async function doQuery(
  hostname: string,
  type: RecordType,
): Promise<DNSAnswer[]> {
  // Use custom endpoint if set, otherwise use provider
  const baseUrl = customDoHEndpoint ?? DOH_PROVIDERS[currentProvider]
  const url = new URL(baseUrl)

  // Cloudflare-style DoH uses type as string, Google-style uses numeric
  const isCloudflareStyle =
    customDoHEndpoint === null
      ? currentProvider === 'cloudflare'
      : baseUrl.includes('cloudflare')

  url.searchParams.set('name', hostname)
  if (isCloudflareStyle) {
    url.searchParams.set('type', type)
  } else {
    url.searchParams.set('type', String(RECORD_TYPES[type]))
  }

  const response = await fetch(url.toString(), {
    headers: {
      Accept: 'application/dns-json',
    },
  })

  if (!response.ok) {
    throw new Error(
      `DNS query failed: ${response.status} ${response.statusText}`,
    )
  }

  const data: DNSResponse = await response.json()

  if (data.Status !== 0) {
    // NXDOMAIN or other DNS error
    const errorCodes: Record<number, string> = {
      1: 'Format error',
      2: 'Server failure',
      3: 'NXDOMAIN - domain does not exist',
      4: 'Not implemented',
      5: 'Refused',
    }
    throw new Error(
      `DNS error: ${errorCodes[data.Status] ?? `Unknown error (${data.Status})`}`,
    )
  }

  return data.Answer ?? []
}

export async function lookup(
  hostname: string,
  options?: { family?: 4 | 6 | 0; all?: boolean },
): Promise<string | string[] | { address: string; family: 4 | 6 }[]> {
  const family = options?.family ?? 0
  const all = options?.all ?? false

  const results: { address: string; family: 4 | 6 }[] = []
  let lastError: Error | null = null

  // Get IPv4 addresses
  if (family === 0 || family === 4) {
    try {
      const answers = await doQuery(hostname, 'A')
      for (const answer of answers) {
        if (answer.type === RECORD_TYPES.A) {
          results.push({ address: answer.data, family: 4 })
        }
      }
    } catch (err) {
      // Only store error if we're exclusively looking for IPv4
      // or if this is the first error
      if (family === 4) {
        throw err
      }
      lastError = err instanceof Error ? err : new Error(String(err))
    }
  }

  // Get IPv6 addresses
  if (family === 0 || family === 6) {
    try {
      const answers = await doQuery(hostname, 'AAAA')
      for (const answer of answers) {
        if (answer.type === RECORD_TYPES.AAAA) {
          results.push({ address: answer.data, family: 6 })
        }
      }
    } catch (err) {
      // Only store error if we're exclusively looking for IPv6
      if (family === 6) {
        throw err
      }
      // If A records also failed, preserve the first error
      if (lastError === null) {
        lastError = err instanceof Error ? err : new Error(String(err))
      }
    }
  }

  if (results.length === 0) {
    // Throw original error if we have one, otherwise generic ENOTFOUND
    if (lastError !== null) {
      throw lastError
    }
    throw new Error(`ENOTFOUND: DNS lookup failed for ${hostname}`)
  }

  if (all) {
    return results
  }

  // Return first result (guaranteed to exist due to length check above)
  return results[0]!.address
}

export async function resolve(
  hostname: string,
  rrtype: RecordType = 'A',
): Promise<string[]> {
  const answers = await doQuery(hostname, rrtype)
  return answers
    .filter((a) => a.type === RECORD_TYPES[rrtype])
    .map((a) => a.data)
}

export async function resolve4(hostname: string): Promise<string[]> {
  return resolve(hostname, 'A')
}

export async function resolve6(hostname: string): Promise<string[]> {
  return resolve(hostname, 'AAAA')
}

export async function resolveCname(hostname: string): Promise<string[]> {
  return resolve(hostname, 'CNAME')
}

/**
 * Resolve MX records
 */
export async function resolveMx(
  hostname: string,
): Promise<Array<{ priority: number; exchange: string }>> {
  const answers = await doQuery(hostname, 'MX')
  return answers
    .filter((a) => a.type === RECORD_TYPES.MX)
    .map((a) => {
      // MX data format: "priority exchange"
      const parts = a.data.split(' ')
      const priorityStr = parts[0] ?? '0'
      return {
        priority: parseInt(priorityStr, 10),
        exchange: parts.slice(1).join(' '),
      }
    })
    .sort((a, b) => a.priority - b.priority)
}

/**
 * Resolve TXT records
 */
export async function resolveTxt(hostname: string): Promise<string[][]> {
  const answers = await doQuery(hostname, 'TXT')
  return answers
    .filter((a) => a.type === RECORD_TYPES.TXT)
    .map((a) => [a.data.replace(/^"|"$/g, '')]) // Remove surrounding quotes
}

/**
 * Resolve NS records
 */
export async function resolveNs(hostname: string): Promise<string[]> {
  return resolve(hostname, 'NS')
}

/**
 * Resolve SRV records
 */
export async function resolveSrv(
  hostname: string,
): Promise<
  Array<{ name: string; port: number; priority: number; weight: number }>
> {
  const answers = await doQuery(hostname, 'SRV')
  return answers
    .filter((a) => a.type === RECORD_TYPES.SRV)
    .map((a) => {
      // SRV data format: "priority weight port target"
      const parts = a.data.split(' ')
      return {
        priority: parseInt(parts[0] ?? '0', 10),
        weight: parseInt(parts[1] ?? '0', 10),
        port: parseInt(parts[2] ?? '0', 10),
        name: parts[3] ?? '',
      }
    })
}

/**
 * Reverse DNS lookup
 */
export async function reverse(ip: string): Promise<string[]> {
  // Convert IP to reverse DNS format
  let reverseAddr: string

  if (ip.includes(':')) {
    // IPv6
    const expanded = ip
      .split(':')
      .map((part) => part.padStart(4, '0'))
      .join('')
    reverseAddr = `${expanded.split('').reverse().join('.')}.ip6.arpa`
  } else {
    // IPv4
    reverseAddr = `${ip.split('.').reverse().join('.')}.in-addr.arpa`
  }

  return resolve(reverseAddr, 'PTR')
}

/**
 * Get DNS servers (returns DoH provider URL)
 */
export function getServers(): string[] {
  if (customDoHEndpoint !== null) {
    return [customDoHEndpoint]
  }
  return [DOH_PROVIDERS[currentProvider]]
}

// Set DoH endpoint. Accepts https:// URLs. IP addresses are ignored (DoH requires HTTPS).
export function setServers(servers: string[]): void {
  if (servers.length === 0) return

  const server = servers[0]
  if (server === undefined) return

  // If it's an HTTPS URL, try to use it as a DoH endpoint
  if (server.startsWith('https://')) {
    // Check for known providers first
    if (server.includes('cloudflare')) {
      currentProvider = 'cloudflare'
      customDoHEndpoint = null
    } else if (server.includes('google')) {
      currentProvider = 'google'
      customDoHEndpoint = null
    } else {
      // Use as custom DoH endpoint
      customDoHEndpoint = server
    }
  } else if (server.match(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)) {
    // IP address - cannot use with DoH, warn and ignore
    console.warn(
      `[dns.setServers] IP address "${server}" cannot be used with DNS-over-HTTPS. ` +
        'Provide a DoH endpoint URL (https://...) or use a known provider.',
    )
  } else if (server.startsWith('http://')) {
    // Non-secure HTTP - warn about security
    console.warn(
      `[dns.setServers] Non-HTTPS URL "${server}" is not supported. ` +
        'DNS-over-HTTPS requires a secure HTTPS endpoint.',
    )
  } else {
    // Unrecognized format
    console.warn(
      `[dns.setServers] Unrecognized server format: "${server}". ` +
        'Expected an HTTPS URL like "https://cloudflare-dns.com/dns-query".',
    )
  }
}

export function resetServers(): void {
  currentProvider = 'cloudflare'
  customDoHEndpoint = null
}

// Default export matching bun:dns module structure
export default {
  lookup,
  resolve,
  resolve4,
  resolve6,
  resolveCname,
  resolveMx,
  resolveTxt,
  resolveNs,
  resolveSrv,
  reverse,
  getServers,
  setServers,
  resetServers,
  setProvider,
  getProvider,
}
