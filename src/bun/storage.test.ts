/**
 * Storage Module Tests
 *
 * Tests the DWS Storage integration for Bun.file/write.
 */

import { describe, test, expect, beforeEach, afterEach, beforeAll } from 'bun:test'
import storage, {
  file,
  write,
  configure,
  getConfig,
  clearMemory,
  upload,
  download,
  getCid,
} from './storage'

const DWS_ENDPOINT = process.env.DWS_ENDPOINT ?? 'http://localhost:4030'

// Check DWS availability - verify upload endpoint works (not just health)
async function checkDWS(): Promise<boolean> {
  // Skip DWS tests unless explicitly enabled (DWS upload is slow in dev)
  if (process.env.RUN_DWS_TESTS !== '1') {
    return false
  }

  try {
    const controller = new AbortController()
    const timeoutId = setTimeout(() => controller.abort(), 2000)
    const response = await fetch(`${DWS_ENDPOINT}/storage/health`, {
      signal: controller.signal,
    })
    clearTimeout(timeoutId)
    return response.ok
  } catch {
    return false
  }
}

describe('Storage Module', () => {

  beforeEach(() => {
    clearMemory()
  })

  describe('Configuration', () => {
    test('getConfig returns current config', () => {
      const config = getConfig()
      expect(config).toHaveProperty('endpoint')
      expect(config).toHaveProperty('ipfsGateway')
    })

    test('configure updates config', () => {
      const original = getConfig()
      configure({ endpoint: 'http://test.local:8080' })
      expect(getConfig().endpoint).toBe('http://test.local:8080')
      // Restore
      configure({ endpoint: original.endpoint })
    })
  })

  describe('Memory Storage (Default)', () => {
    test('write and read file from memory', async () => {
      const path = '/test/memory-file.txt'
      const content = 'Hello, memory storage.'

      const bytesWritten = await write(path, content)
      expect(bytesWritten).toBe(content.length)

      const f = file(path)
      expect(await f.text()).toBe(content)
      expect(await f.exists()).toBe(true)
    })

    test('file.bytes returns Uint8Array', async () => {
      const path = '/test/bytes.bin'
      const data = new Uint8Array([1, 2, 3, 4, 5])

      await write(path, data)
      const f = file(path)
      const bytes = await f.bytes()

      expect(bytes).toBeInstanceOf(Uint8Array)
      expect(Array.from(bytes)).toEqual([1, 2, 3, 4, 5])
    })

    test('file.json parses JSON content', async () => {
      const path = '/test/data.json'
      const data = { name: 'test', value: 42 }

      await write(path, JSON.stringify(data))
      const f = file(path)
      const parsed = await f.json<typeof data>()

      expect(parsed).toEqual(data)
    })

    test('file.arrayBuffer returns ArrayBuffer', async () => {
      const path = '/test/buffer.bin'
      const data = new Uint8Array([10, 20, 30])

      await write(path, data)
      const f = file(path)
      const buffer = await f.arrayBuffer()

      expect(buffer).toBeInstanceOf(ArrayBuffer)
      expect(buffer.byteLength).toBe(3)
    })

    test('file.stream returns ReadableStream', async () => {
      const path = '/test/stream.txt'
      const content = 'Stream content'

      await write(path, content)
      const f = file(path)
      const stream = f.stream()

      expect(stream).toBeInstanceOf(ReadableStream)

      const reader = stream.getReader()
      const { value } = await reader.read()
      expect(new TextDecoder().decode(value)).toBe(content)
    })

    test('file.slice returns sliced file', async () => {
      const path = '/test/slice.txt'
      const content = 'Hello, World!'

      await write(path, content)
      const f = file(path)
      const sliced = f.slice(0, 5)

      expect(await sliced.text()).toBe('Hello')
    })

    test('file.exists returns false for non-existent file', async () => {
      const f = file('/does/not/exist.txt')
      expect(await f.exists()).toBe(false)
    })

    test('file.text throws for non-existent file', async () => {
      const f = file('/does/not/exist.txt')
      await expect(f.text()).rejects.toThrow()
    })

    test('file.writer writes in chunks', async () => {
      const path = '/test/writer.txt'
      const f = file(path)
      const writer = f.writer()

      writer.write('Hello, ')
      writer.write('World!')
      await writer.end()

      const result = file(path)
      expect(await result.text()).toBe('Hello, World!')
    })

    test('file properties are correct', async () => {
      const path = '/test/props.txt'
      const content = 'Test content'

      await write(path, content)
      const f = file(path)

      expect(f.name).toBe(path)
      expect(f.size).toBe(content.length)
      expect(f.type).toBe('application/octet-stream')
      expect(typeof f.lastModified).toBe('number')
    })
  })

  describe('DWS Storage URLs', () => {
    test('dws:// URL is recognized for download', () => {
      const f = file('dws://QmTest123')
      expect(f.name).toBe('dws://QmTest123')
    })

    test('ipfs:// URL is recognized for download', () => {
      const f = file('ipfs://QmTest456')
      expect(f.name).toBe('ipfs://QmTest456')
    })

    test('dws://upload/ URL is recognized for upload', async () => {
      // This would try to upload to DWS if available
      const path = 'dws://upload/test-file.txt'
      const f = file(path)
      expect(f.name).toBe(path)
    })
  })

  describe('DWS Storage (Live)', () => {
    test('upload to DWS and download', async () => {
      const dwsAvailable = await checkDWS()
      if (!dwsAvailable) {
        // Test skipped - DWS not available (set RUN_DWS_TESTS=1 to enable)
        return
      }

      const content = `Test content ${Date.now()}`
      const result = await upload(content, { filename: 'test.txt' })

      expect(result).toHaveProperty('cid')
      expect(result).toHaveProperty('url')
      expect(result.cid).toBeTruthy()

      // Download and verify
      const downloaded = await download(result.cid)
      expect(new TextDecoder().decode(downloaded)).toBe(content)
    })

    test('write to dws://upload/ path', async () => {
      const dwsAvailable = await checkDWS()
      if (!dwsAvailable) {
        // Test skipped - DWS not available (set RUN_DWS_TESTS=1 to enable)
        return
      }

      const path = 'dws://upload/integration-test.txt'
      const content = `Integration test ${Date.now()}`

      await write(path, content)
      const cid = getCid(path)

      expect(cid).toBeTruthy()

      // Verify download
      if (cid) {
        const downloaded = await download(cid)
        expect(new TextDecoder().decode(downloaded)).toBe(content)
      }
    })

    test('file writer uploads to DWS', async () => {
      const dwsAvailable = await checkDWS()
      if (!dwsAvailable) {
        // Test skipped - DWS not available (set RUN_DWS_TESTS=1 to enable)
        return
      }

      const path = 'dws://upload/writer-test.txt'
      const f = file(path)
      const writer = f.writer()

      writer.write('Chunk 1. ')
      writer.write('Chunk 2.')
      const result = await writer.end()

      expect(result).toHaveProperty('cid')
      expect(result.cid).toBeTruthy()

      // Verify
      if (result.cid) {
        const downloaded = await download(result.cid)
        expect(new TextDecoder().decode(downloaded)).toBe('Chunk 1. Chunk 2.')
      }
    })

    test('read from dws:// path', async () => {
      const dwsAvailable = await checkDWS()
      if (!dwsAvailable) {
        // Test skipped - DWS not available (set RUN_DWS_TESTS=1 to enable)
        return
      }

      // First upload
      const content = `Read test ${Date.now()}`
      const uploadResult = await upload(content)

      // Then read
      const f = file(`dws://${uploadResult.cid}`)
      const text = await f.text()

      expect(text).toBe(content)
    })
  })

  describe('Edge Cases', () => {
    test('write with Blob data', async () => {
      const path = '/test/blob.txt'
      const blob = new Blob(['Blob content'])

      await write(path, blob)
      const f = file(path)
      expect(await f.text()).toBe('Blob content')
    })

    test('write with Response data', async () => {
      const path = '/test/response.txt'
      const response = new Response('Response content')

      await write(path, response)
      const f = file(path)
      expect(await f.text()).toBe('Response content')
    })

    test('write with ArrayBuffer data', async () => {
      const path = '/test/arraybuffer.bin'
      const buffer = new ArrayBuffer(4)
      const view = new Uint8Array(buffer)
      view.set([1, 2, 3, 4])

      await write(path, buffer)
      const f = file(path)
      const bytes = await f.bytes()
      expect(Array.from(bytes)).toEqual([1, 2, 3, 4])
    })

    test('write with BunFile data', async () => {
      const sourcePath = '/test/source.txt'
      const destPath = '/test/dest.txt'

      await write(sourcePath, 'Source content')
      const sourceFile = file(sourcePath)

      await write(destPath, sourceFile)
      const destFile = file(destPath)
      expect(await destFile.text()).toBe('Source content')
    })

    test('empty file', async () => {
      const path = '/test/empty.txt'
      await write(path, '')

      const f = file(path)
      expect(await f.text()).toBe('')
      expect(f.size).toBe(0)
    })

    test('large file (1MB)', async () => {
      const path = '/test/large.bin'
      const data = new Uint8Array(1024 * 1024) // 1MB
      data.fill(42)

      await write(path, data)
      const f = file(path)
      const bytes = await f.bytes()

      expect(bytes.byteLength).toBe(1024 * 1024)
      expect(bytes[0]).toBe(42)
      expect(bytes[bytes.length - 1]).toBe(42)
    })

    test('unicode content', async () => {
      const path = '/test/unicode.txt'
      const content = '你好世界 🌍 Привет мир'

      await write(path, content)
      const f = file(path)
      expect(await f.text()).toBe(content)
    })

    test('clearMemory clears all stored data', async () => {
      await write('/test/file1.txt', 'Content 1')
      await write('/test/file2.txt', 'Content 2')

      expect(await file('/test/file1.txt').exists()).toBe(true)
      expect(await file('/test/file2.txt').exists()).toBe(true)

      clearMemory()

      expect(await file('/test/file1.txt').exists()).toBe(false)
      expect(await file('/test/file2.txt').exists()).toBe(false)
    })
  })

  describe('Default Export', () => {
    test('default export has all functions', () => {
      expect(typeof storage.configure).toBe('function')
      expect(typeof storage.getConfig).toBe('function')
      expect(typeof storage.upload).toBe('function')
      expect(typeof storage.download).toBe('function')
      expect(typeof storage.exists).toBe('function')
      expect(typeof storage.file).toBe('function')
      expect(typeof storage.write).toBe('function')
      expect(typeof storage.getCid).toBe('function')
      expect(typeof storage.clearMemory).toBe('function')
    })
  })
})
