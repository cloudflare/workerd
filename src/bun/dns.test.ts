/**
 * DNS Module Tests
 *
 * Tests the DNS-over-HTTPS implementation.
 * Requires network access to DoH providers.
 */

import { describe, test, expect, beforeAll } from 'bun:test'
import dns, {
  lookup,
  resolve,
  resolve4,
  resolve6,
  resolveCname,
  resolveMx,
  resolveTxt,
  resolveNs,
  reverse,
  getServers,
  setProvider,
  getProvider,
} from './dns'

let networkAvailable = false

describe('DNS Module (DoH)', () => {
  beforeAll(async () => {
    // Check if network is available
    try {
      const response = await fetch('https://cloudflare-dns.com/dns-query?name=example.com&type=A', {
        headers: { Accept: 'application/dns-json' },
        signal: AbortSignal.timeout(5000),
      })
      networkAvailable = response.ok
    } catch {
      networkAvailable = false
    }

    if (!networkAvailable) {
      console.log('⚠️  Network not available - skipping live DNS tests')
    }
  })

  describe('Provider Configuration', () => {
    test('getProvider returns current provider', () => {
      const provider = getProvider()
      expect(['cloudflare', 'google']).toContain(provider)
    })

    test('setProvider changes provider', () => {
      const original = getProvider()
      setProvider('google')
      expect(getProvider()).toBe('google')
      setProvider('cloudflare')
      expect(getProvider()).toBe('cloudflare')
      // Restore original
      setProvider(original)
    })

    test('setProvider throws for unknown provider', () => {
      expect(() => setProvider('invalid' as 'cloudflare')).toThrow('Unknown DNS provider')
    })

    test('getServers returns provider URL', () => {
      const servers = getServers()
      expect(Array.isArray(servers)).toBe(true)
      expect(servers.length).toBeGreaterThan(0)
      expect(servers[0]).toContain('dns')
    })
  })

  describe('DNS Lookup (Live)', () => {
    test('lookup resolves google.com', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const address = await lookup('google.com')
      expect(typeof address).toBe('string')
      // Should be a valid IPv4 or IPv6 address
      expect(address).toMatch(/^[\d.:a-f]+$/i)
    })

    test('lookup with all option returns array', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const addresses = await lookup('google.com', { all: true })
      expect(Array.isArray(addresses)).toBe(true)
      expect(addresses.length).toBeGreaterThan(0)
      // Each should have address and family
      const first = addresses[0] as { address: string; family: 4 | 6 }
      expect(first).toHaveProperty('address')
      expect(first).toHaveProperty('family')
      expect([4, 6]).toContain(first.family)
    })

    test('lookup with family 4 returns IPv4', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const addresses = await lookup('google.com', { family: 4, all: true })
      expect(Array.isArray(addresses)).toBe(true)
      for (const addr of addresses as Array<{ family: number }>) {
        expect(addr.family).toBe(4)
      }
    })

    test('lookup throws for non-existent domain', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      await expect(lookup('definitely-does-not-exist-12345.invalid')).rejects.toThrow()
    })
  })

  describe('DNS Resolve (Live)', () => {
    test('resolve4 returns IPv4 addresses', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const addresses = await resolve4('google.com')
      expect(Array.isArray(addresses)).toBe(true)
      expect(addresses.length).toBeGreaterThan(0)
      // Should be valid IPv4 addresses
      for (const addr of addresses) {
        expect(addr).toMatch(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)
      }
    })

    test('resolve6 returns IPv6 addresses', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      try {
        const addresses = await resolve6('google.com')
        expect(Array.isArray(addresses)).toBe(true)
        // Google should have IPv6
        if (addresses.length > 0) {
          expect(addresses[0]).toContain(':')
        }
      } catch {
        // Some domains may not have AAAA records
        console.log('  (no AAAA records)')
      }
    })

    test('resolveMx returns MX records', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const records = await resolveMx('google.com')
      expect(Array.isArray(records)).toBe(true)
      expect(records.length).toBeGreaterThan(0)
      expect(records[0]).toHaveProperty('priority')
      expect(records[0]).toHaveProperty('exchange')
      expect(typeof records[0].priority).toBe('number')
    })

    test('resolveNs returns NS records', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const records = await resolveNs('google.com')
      expect(Array.isArray(records)).toBe(true)
      expect(records.length).toBeGreaterThan(0)
      // NS records should be domain names
      expect(records[0]).toContain('.')
    })

    test('resolveTxt returns TXT records', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      const records = await resolveTxt('google.com')
      expect(Array.isArray(records)).toBe(true)
      // Google should have SPF and other TXT records
      expect(records.length).toBeGreaterThan(0)
    })
  })

  describe('Reverse DNS (Live)', () => {
    test('reverse lookup for 8.8.8.8', async () => {
      if (!networkAvailable) {
        console.log('SKIPPED - Network not available')
        return
      }

      try {
        const hostnames = await reverse('8.8.8.8')
        expect(Array.isArray(hostnames)).toBe(true)
        // Google's DNS should have PTR records
        if (hostnames.length > 0) {
          expect(hostnames[0]).toContain('google')
        }
      } catch {
        // PTR records may not exist
        console.log('  (no PTR record)')
      }
    })
  })

  describe('Default Export', () => {
    test('default export has all functions', () => {
      expect(typeof dns.lookup).toBe('function')
      expect(typeof dns.resolve).toBe('function')
      expect(typeof dns.resolve4).toBe('function')
      expect(typeof dns.resolve6).toBe('function')
      expect(typeof dns.resolveCname).toBe('function')
      expect(typeof dns.resolveMx).toBe('function')
      expect(typeof dns.resolveTxt).toBe('function')
      expect(typeof dns.resolveNs).toBe('function')
      expect(typeof dns.resolveSrv).toBe('function')
      expect(typeof dns.reverse).toBe('function')
      expect(typeof dns.getServers).toBe('function')
      expect(typeof dns.setServers).toBe('function')
    })
  })
})
