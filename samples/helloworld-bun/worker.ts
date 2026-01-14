// Bun Hello World - Demonstrates Bun APIs in workerd
// Run: workerd serve config.capnp
// Test: curl http://localhost:9124/

import Bun from './bun-bundle.js'

const startTime = Date.now()

const json = (data: unknown, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { 'content-type': 'application/json' },
  })

export default {
  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url)

    switch (url.pathname) {
      case '/':
        return json({
          message: 'Hello from Bun!',
          bunVersion: Bun.version,
          uptime: Date.now() - startTime,
          timestamp: new Date().toISOString(),
        })

      case '/hash': {
        const data = url.searchParams.get('data') ?? 'hello'
        return json({ input: data, hash: Bun.hash(data).toString(16) })
      }

      case '/deep-equals': {
        const obj1 = { a: 1, b: { c: [1, 2, 3] } }
        const obj2 = { a: 1, b: { c: [1, 2, 3] } }
        const obj3 = { a: 1, b: { c: [1, 2, 4] } }
        return json({
          'obj1 === obj2': Bun.deepEquals(obj1, obj2),
          'obj1 === obj3': Bun.deepEquals(obj1, obj3),
        })
      }

      case '/escape-html': {
        const html = url.searchParams.get('html') ?? '<script>alert(1)</script>'
        return json({ input: html, escaped: Bun.escapeHTML(html) })
      }

      case '/nanoseconds':
        return json({ nanoseconds: Bun.nanoseconds().toString() })

      case '/inspect': {
        const obj = { name: 'test', nested: { deep: { value: [1, 2, 3] } } }
        return json({ inspected: Bun.inspect(obj) })
      }

      case '/string-width':
        return json({
          results: ['hello', '你好', '🎉'].map((s) => ({
            string: s,
            width: Bun.stringWidth(s),
          })),
        })

      case '/array-buffer-sink': {
        const sink = new Bun.ArrayBufferSink()
        sink.write('Hello ')
        sink.write('World')
        const buffer = sink.end()
        return json({
          text: new TextDecoder().decode(buffer),
          byteLength: buffer.byteLength,
        })
      }

      case '/stream': {
        const stream = new ReadableStream({
          start(c) {
            c.enqueue(new TextEncoder().encode('Stream '))
            c.enqueue(new TextEncoder().encode('content'))
            c.close()
          },
        })
        return json({ text: await Bun.readableStreamToText(stream) })
      }

      case '/sleep': {
        const ms = parseInt(url.searchParams.get('ms') ?? '100')
        const before = Date.now()
        await Bun.sleep(ms)
        return json({ requestedMs: ms, actualMs: Date.now() - before })
      }

      case '/file-ops': {
        const path = '/tmp/bun-test.txt'
        const existedBefore = await Bun.file(path).exists()
        await Bun.write(path, 'Hello from Bun file API.')
        const file = Bun.file(path)
        return json({
          written: true,
          existedBefore,
          content: await file.text(),
          size: file.size,
        })
      }

      case '/password-hash': {
        const body = (await request.json()) as { password?: string; cost?: number }
        const password = body.password ?? 'default'
        const cost = body.cost ?? 10
        const hash = await Bun.password.hash(password, { cost })
        return json({ hash, algorithm: 'pbkdf2', cost })
      }

      case '/password-verify': {
        const body = (await request.json()) as { password?: string; hash?: string }
        if (!body.password || !body.hash) {
          return json({ error: 'Missing password or hash' }, 400)
        }
        return json({ valid: await Bun.password.verify(body.password, body.hash) })
      }

      case '/dns-lookup': {
        const hostname = url.searchParams.get('hostname') ?? 'google.com'
        const address = await Bun.dns.lookup(hostname)
        return json({ hostname, address, provider: Bun.dns.getProvider() })
      }

      case '/dns-resolve': {
        const hostname = url.searchParams.get('hostname') ?? 'google.com'
        const type = url.searchParams.get('type') ?? 'A'
        let records: string[] | Array<{ exchange: string; priority: number }>
        switch (type) {
          case 'MX':
            records = await Bun.dns.resolveMx(hostname)
            break
          case 'TXT':
            records = await Bun.dns.resolveTxt(hostname)
            break
          case 'NS':
            records = await Bun.dns.resolveNs(hostname)
            break
          case 'AAAA':
            records = await Bun.dns.resolve6(hostname)
            break
          default:
            records = await Bun.dns.resolve4(hostname)
        }
        return json({ hostname, type, records })
      }

      case '/health':
        return new Response('OK')

      default:
        return json(
          {
            error: 'Not Found',
            path: url.pathname,
            routes: [
              '/',
              '/hash',
              '/deep-equals',
              '/escape-html',
              '/nanoseconds',
              '/inspect',
              '/string-width',
              '/array-buffer-sink',
              '/stream',
              '/sleep',
              '/file-ops',
              '/password-hash',
              '/password-verify',
              '/dns-lookup',
              '/dns-resolve',
              '/health',
            ],
          },
          404,
        )
    }
  },
}
