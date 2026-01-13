// Copyright (c) 2024 Jeju Network
// Bun-compatible worker sample for workerd
// Licensed under the Apache 2.0 license

// This worker demonstrates native bun:* imports in workerd
// Requires workerd built from source with Bun compatibility

import Bun from 'bun:bun'

const startTime = Date.now()

export default {
  async fetch(request) {
    const url = new URL(request.url)
    
    switch (url.pathname) {
      case '/':
        return new Response(JSON.stringify({
          message: 'Hello from Bun worker!',
          runtime: 'workerd',
          bunVersion: Bun.version,
          uptime: Date.now() - startTime,
          timestamp: new Date().toISOString()
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/bun-version':
        return new Response(JSON.stringify({
          version: Bun.version,
          revision: Bun.revision
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/hash':
        // Test Bun.hash
        const data = url.searchParams.get('data') || 'hello'
        const hash = Bun.hash(data)
        
        return new Response(JSON.stringify({
          input: data,
          hash: hash.toString(16)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/deep-equals':
        // Test Bun.deepEquals
        const obj1 = { a: 1, b: { c: [1, 2, 3] } }
        const obj2 = { a: 1, b: { c: [1, 2, 3] } }
        const obj3 = { a: 1, b: { c: [1, 2, 4] } }
        
        return new Response(JSON.stringify({
          obj1_equals_obj2: Bun.deepEquals(obj1, obj2),
          obj1_equals_obj3: Bun.deepEquals(obj1, obj3)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/escape-html':
        // Test Bun.escapeHTML
        const html = url.searchParams.get('html') || '<script>alert("xss")</script>'
        
        return new Response(JSON.stringify({
          input: html,
          escaped: Bun.escapeHTML(html)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/nanoseconds':
        // Test Bun.nanoseconds
        const ns = Bun.nanoseconds()
        
        return new Response(JSON.stringify({
          nanoseconds: ns.toString()
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/file-ops':
        // Test Bun.file and Bun.write
        await Bun.write('/test.txt', 'Hello from Bun file API!')
        const file = Bun.file('/test.txt')
        const content = await file.text()
        const exists = await file.exists()
        
        return new Response(JSON.stringify({
          written: true,
          content,
          exists,
          size: file.size
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/inspect':
        // Test Bun.inspect
        const complexObj = {
          name: 'test',
          nested: { deep: { value: [1, 2, 3] } },
          date: new Date()
        }
        
        return new Response(JSON.stringify({
          inspected: Bun.inspect(complexObj)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/echo':
        const body = await request.text()
        return new Response(JSON.stringify({
          method: request.method,
          url: request.url,
          headers: Object.fromEntries(request.headers),
          body: body || null
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/headers':
        const respHeaders = new Headers()
        respHeaders.set('X-Custom-Header', 'Bun Worker')
        respHeaders.set('X-Request-ID', crypto.randomUUID())
        
        return new Response(JSON.stringify({
          requestHeaders: Object.fromEntries(request.headers),
          customHeaders: Object.fromEntries(respHeaders)
        }), {
          headers: {
            'content-type': 'application/json',
            ...Object.fromEntries(respHeaders)
          }
        })
      
      case '/stream':
        const stream = new ReadableStream({
          start(controller) {
            const chunks = ['Hello', ' ', 'from', ' ', 'Bun', ' ', 'streaming', '!']
            let i = 0
            
            const intervalId = setInterval(() => {
              if (i < chunks.length) {
                controller.enqueue(new TextEncoder().encode(chunks[i]))
                i++
              } else {
                clearInterval(intervalId)
                controller.close()
              }
            }, 100)
          }
        })
        
        return new Response(stream, {
          headers: { 'content-type': 'text/plain' }
        })
      
      case '/health':
        return new Response('OK', {
          status: 200,
          headers: { 'content-type': 'text/plain' }
        })
      
      default:
        return new Response(JSON.stringify({
          error: 'Not Found',
          path: url.pathname,
          availableRoutes: [
            '/',
            '/bun-version',
            '/hash',
            '/deep-equals',
            '/escape-html',
            '/nanoseconds',
            '/file-ops',
            '/inspect',
            '/echo',
            '/headers',
            '/stream',
            '/health'
          ]
        }), {
          status: 404,
          headers: { 'content-type': 'application/json' }
        })
    }
  }
}
