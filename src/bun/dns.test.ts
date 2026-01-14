/**
 * DNS Module Tests
 *
 * Tests the DNS-over-HTTPS implementation.
 * Live DNS tests require network access to DoH providers.
 */

import { afterEach, beforeAll, describe, expect, test } from 'bun:test'
import dns, {
  getProvider,
  getServers,
  lookup,
  resetServers,
  resolve4,
  resolve6,
  resolveMx,
  resolveNs,
  resolveSrv,
  resolveTxt,
  reverse,
  setProvider,
  setServers,
} from './dns'

// Check network availability once at module load
let networkAvailable = false
async function checkNetwork(): Promise<boolean> {
  try {
    const response = await fetch(
      'https://cloudflare-dns.com/dns-query?name=example.com&type=A',
      {
        headers: { Accept: 'application/dns-json' },
        signal: AbortSignal.timeout(5000),
      },
    )
    return response.ok
  } catch {
    return false
  }
}

describe('DNS Module (DoH)', () => {
  beforeAll(async () => {
    networkAvailable = await checkNetwork()
    if (!networkAvailable) {
      console.log('⚠️  Network not available - live DNS tests will be skipped')
    }
  })

  afterEach(() => {
    // Reset DNS configuration after each test
    resetServers()
  })

  describe('Provider Configuration', () => {
    test('getProvider returns current provider', () => {
      const provider = getProvider()
      expect(['cloudflare', 'google']).toContain(provider)
    })

    test('setProvider changes provider', () => {
      setProvider('google')
      expect(getProvider()).toBe('google')
      setProvider('cloudflare')
      expect(getProvider()).toBe('cloudflare')
    })

    test('setProvider throws for unknown provider', () => {
      expect(() => setProvider('invalid' as 'cloudflare')).toThrow(
        'Unknown DNS provider',
      )
    })

    test('getServers returns provider URL', () => {
      const servers = getServers()
      expect(Array.isArray(servers)).toBe(true)
      expect(servers.length).toBe(1)
      expect(servers[0]).toContain('dns')
    })

    test('getServers reflects provider change', () => {
      setProvider('cloudflare')
      expect(getServers()[0]).toContain('cloudflare')

      setProvider('google')
      expect(getServers()[0]).toContain('google')
    })
  })

  describe('setServers Configuration', () => {
    test('setServers with cloudflare URL sets cloudflare provider', () => {
      setServers(['https://cloudflare-dns.com/dns-query'])
      expect(getProvider()).toBe('cloudflare')
    })

    test('setServers with google URL sets google provider', () => {
      setServers(['https://dns.google/resolve'])
      expect(getProvider()).toBe('google')
    })

    test('setServers with custom URL stores custom endpoint', () => {
      const customUrl = 'https://custom-doh.example.com/dns-query'
      setServers([customUrl])
      expect(getServers()[0]).toBe(customUrl)
    })

    test('setServers ignores empty array', () => {
      setProvider('google')
      setServers([])
      expect(getProvider()).toBe('google')
    })

    test('setServers warns for IP address', () => {
      // IP addresses can't be used with DoH - should warn but not crash
      const originalWarn = console.warn
      let warnCalled = false
      console.warn = () => {
        warnCalled = true
      }

      setServers(['8.8.8.8'])
      expect(warnCalled).toBe(true)

      console.warn = originalWarn
    })

    test('setServers uses first server only', () => {
      setServers([
        'https://custom1.example.com/dns',
        'https://custom2.example.com/dns',
      ])
      expect(getServers()[0]).toBe('https://custom1.example.com/dns')
    })
  })

  describe('resetServers', () => {
    test('resetServers restores default cloudflare provider', () => {
      setProvider('google')
      resetServers()
      expect(getProvider()).toBe('cloudflare')
    })

    test('resetServers clears custom endpoint', () => {
      setServers(['https://custom.example.com/dns'])
      resetServers()
      expect(getServers()[0]).toContain('cloudflare')
    })
  })

  describe('DNS Lookup (Live)', () => {
    test.skipIf(!networkAvailable)('lookup resolves google.com', async () => {
      const address = await lookup('google.com')
      expect(typeof address).toBe('string')
      // Should be a valid IPv4 or IPv6 address
      expect(address).toMatch(/^[\d.:a-f]+$/i)
    })

    test.skipIf(!networkAvailable)(
      'lookup with all option returns array',
      async () => {
        const addresses = await lookup('google.com', { all: true })
        expect(Array.isArray(addresses)).toBe(true)
        expect(addresses.length).toBeGreaterThan(0)
        const first = addresses[0] as { address: string; family: 4 | 6 }
        expect(first).toHaveProperty('address')
        expect(first).toHaveProperty('family')
        expect([4, 6]).toContain(first.family)
      },
    )

    test.skipIf(!networkAvailable)(
      'lookup with family 4 returns IPv4',
      async () => {
        const addresses = await lookup('google.com', { family: 4, all: true })
        expect(Array.isArray(addresses)).toBe(true)
        for (const addr of addresses as Array<{ family: number }>) {
          expect(addr.family).toBe(4)
        }
      },
    )

    test.skipIf(!networkAvailable)(
      'lookup throws for non-existent domain',
      async () => {
        await expect(
          lookup('definitely-does-not-exist-12345.invalid'),
        ).rejects.toThrow()
      },
    )
  })

  describe('DNS Resolve (Live)', () => {
    test.skipIf(!networkAvailable)(
      'resolve4 returns IPv4 addresses',
      async () => {
        const addresses = await resolve4('google.com')
        expect(Array.isArray(addresses)).toBe(true)
        expect(addresses.length).toBeGreaterThan(0)
        for (const addr of addresses) {
          expect(addr).toMatch(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)
        }
      },
    )

    test.skipIf(!networkAvailable)(
      'resolve6 returns IPv6 addresses for google.com',
      async () => {
        const addresses = await resolve6('google.com')
        expect(Array.isArray(addresses)).toBe(true)
        // Google should have IPv6
        if (addresses.length > 0) {
          expect(addresses[0]).toContain(':')
        }
      },
    )

    test.skipIf(!networkAvailable)('resolveMx returns MX records', async () => {
      const records = await resolveMx('google.com')
      expect(Array.isArray(records)).toBe(true)
      expect(records.length).toBeGreaterThan(0)
      expect(records[0]).toHaveProperty('priority')
      expect(records[0]).toHaveProperty('exchange')
      expect(typeof records[0].priority).toBe('number')
    })

    test.skipIf(!networkAvailable)('resolveNs returns NS records', async () => {
      const records = await resolveNs('google.com')
      expect(Array.isArray(records)).toBe(true)
      expect(records.length).toBeGreaterThan(0)
      expect(records[0]).toContain('.')
    })

    test.skipIf(!networkAvailable)(
      'resolveTxt returns TXT records',
      async () => {
        const records = await resolveTxt('google.com')
        expect(Array.isArray(records)).toBe(true)
        expect(records.length).toBeGreaterThan(0)
      },
    )

    test.skipIf(!networkAvailable)(
      'resolveSrv returns SRV records',
      async () => {
        // Test with a domain known to have SRV records
        // _xmpp-server._tcp.gmail.com has SRV records
        const records = await resolveSrv('_xmpp-server._tcp.gmail.com')
        expect(Array.isArray(records)).toBe(true)
        if (records.length > 0) {
          expect(records[0]).toHaveProperty('priority')
          expect(records[0]).toHaveProperty('weight')
          expect(records[0]).toHaveProperty('port')
          expect(records[0]).toHaveProperty('name')
        }
      },
    )

    test.skipIf(!networkAvailable)(
      'resolve with explicit type A returns IPv4',
      async () => {
        const records = await dns.resolve('google.com', 'A')
        expect(Array.isArray(records)).toBe(true)
        expect(records.length).toBeGreaterThan(0)
        expect(records[0]).toMatch(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)
      },
    )
  })

  describe('Reverse DNS (Live)', () => {
    test.skipIf(!networkAvailable)(
      'reverse lookup for 8.8.8.8',
      async () => {
        const hostnames = await reverse('8.8.8.8')
        expect(Array.isArray(hostnames)).toBe(true)
        if (hostnames.length > 0) {
          expect(hostnames[0]).toContain('google')
        }
      },
    )
  })

  describe('Provider-specific behavior', () => {
    test.skipIf(!networkAvailable)(
      'cloudflare provider resolves correctly',
      async () => {
        setProvider('cloudflare')
        const addresses = await resolve4('example.com')
        expect(addresses.length).toBeGreaterThan(0)
      },
    )

    test.skipIf(!networkAvailable)(
      'google provider resolves correctly',
      async () => {
        setProvider('google')
        const addresses = await resolve4('example.com')
        expect(addresses.length).toBeGreaterThan(0)
      },
    )
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
      expect(typeof dns.resetServers).toBe('function')
      expect(typeof dns.setProvider).toBe('function')
      expect(typeof dns.getProvider).toBe('function')
    })
  })
})
