/**
 * bun:dns - DNS Resolution via DNS-over-HTTPS (DoH)
 *
 * This provides REAL DNS resolution in workerd using DNS-over-HTTPS.
 * Works with Cloudflare DNS (1.1.1.1) and Google DNS (8.8.8.8).
 */

// DNS record types
type RecordType = 'A' | 'AAAA' | 'CNAME' | 'MX' | 'TXT' | 'NS' | 'SOA' | 'SRV' | 'PTR'

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

/**
 * Set the DNS-over-HTTPS provider
 */
export function setProvider(provider: DoHProvider): void {
  if (!DOH_PROVIDERS[provider]) {
    throw new Error(`Unknown DNS provider: ${provider}. Use 'cloudflare' or 'google'.`)
  }
  currentProvider = provider
}

/**
 * Get current provider
 */
export function getProvider(): DoHProvider {
  return currentProvider
}

/**
 * Perform a DNS query via DoH
 */
async function doQuery(hostname: string, type: RecordType): Promise<DNSAnswer[]> {
  const url = new URL(DOH_PROVIDERS[currentProvider])

  if (currentProvider === 'cloudflare') {
    url.searchParams.set('name', hostname)
    url.searchParams.set('type', type)
  } else {
    url.searchParams.set('name', hostname)
    url.searchParams.set('type', String(RECORD_TYPES[type]))
  }

  const response = await fetch(url.toString(), {
    headers: {
      Accept: 'application/dns-json',
    },
  })

  if (!response.ok) {
    throw new Error(`DNS query failed: ${response.status} ${response.statusText}`)
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
    throw new Error(`DNS error: ${errorCodes[data.Status] ?? `Unknown error (${data.Status})`}`)
  }

  return data.Answer ?? []
}

/**
 * Lookup IP addresses for a hostname (both IPv4 and IPv6)
 */
export async function lookup(
  hostname: string,
  options?: { family?: 4 | 6 | 0; all?: boolean },
): Promise<string | string[] | { address: string; family: 4 | 6 }[]> {
  const family = options?.family ?? 0
  const all = options?.all ?? false

  const results: { address: string; family: 4 | 6 }[] = []

  // Get IPv4 addresses
  if (family === 0 || family === 4) {
    try {
      const answers = await doQuery(hostname, 'A')
      for (const answer of answers) {
        if (answer.type === RECORD_TYPES.A) {
          results.push({ address: answer.data, family: 4 })
        }
      }
    } catch {
      // Ignore errors for A records if we're also checking AAAA
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
    } catch {
      // Ignore errors for AAAA records
    }
  }

  if (results.length === 0) {
    throw new Error(`ENOTFOUND: DNS lookup failed for ${hostname}`)
  }

  if (all) {
    return results
  }

  // Return first result
  return results[0].address
}

/**
 * Resolve DNS records of a specific type
 */
export async function resolve(hostname: string, rrtype: RecordType = 'A'): Promise<string[]> {
  const answers = await doQuery(hostname, rrtype)
  return answers
    .filter((a) => a.type === RECORD_TYPES[rrtype])
    .map((a) => a.data)
}

/**
 * Resolve IPv4 addresses (A records)
 */
export async function resolve4(hostname: string): Promise<string[]> {
  return resolve(hostname, 'A')
}

/**
 * Resolve IPv6 addresses (AAAA records)
 */
export async function resolve6(hostname: string): Promise<string[]> {
  return resolve(hostname, 'AAAA')
}

/**
 * Resolve CNAME records
 */
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
      return {
        priority: parseInt(parts[0], 10),
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
): Promise<Array<{ name: string; port: number; priority: number; weight: number }>> {
  const answers = await doQuery(hostname, 'SRV')
  return answers
    .filter((a) => a.type === RECORD_TYPES.SRV)
    .map((a) => {
      // SRV data format: "priority weight port target"
      const parts = a.data.split(' ')
      return {
        priority: parseInt(parts[0], 10),
        weight: parseInt(parts[1], 10),
        port: parseInt(parts[2], 10),
        name: parts[3],
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
    reverseAddr = expanded.split('').reverse().join('.') + '.ip6.arpa'
  } else {
    // IPv4
    reverseAddr = ip.split('.').reverse().join('.') + '.in-addr.arpa'
  }

  return resolve(reverseAddr, 'PTR')
}

/**
 * Get DNS servers (returns DoH provider URL)
 */
export function getServers(): string[] {
  return [DOH_PROVIDERS[currentProvider]]
}

/**
 * Set DNS servers (switches provider based on URL)
 */
export function setServers(servers: string[]): void {
  if (servers.length === 0) return

  const server = servers[0]
  if (server.includes('cloudflare')) {
    currentProvider = 'cloudflare'
  } else if (server.includes('google')) {
    currentProvider = 'google'
  }
  // Otherwise keep current provider
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
  setProvider,
  getProvider,
}
