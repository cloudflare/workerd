// Copyright (c) 2024 Jeju Network
// Test runner for Bun compatibility layer
// Runs unit tests and integration tests with workerd

import path from 'node:path'
import { type Subprocess, spawn } from 'bun'

const __dirname = path.dirname(new URL(import.meta.url).pathname)
const WORKERD_URL = 'http://127.0.0.1:9124'
const WORKERD_CONFIG = path.resolve(
  __dirname,
  '../../samples/bun-bundle/config.capnp',
)
const STARTUP_TIMEOUT = 10000

async function checkWorkerdRunning(): Promise<boolean> {
  try {
    const response = await fetch(`${WORKERD_URL}/health`, {
      signal: AbortSignal.timeout(1000),
    })
    return response.ok
  } catch {
    return false
  }
}

async function waitForWorkerd(timeoutMs: number): Promise<void> {
  const startTime = Date.now()
  while (Date.now() - startTime < timeoutMs) {
    if (await checkWorkerdRunning()) {
      return
    }
    await new Promise((resolve) => setTimeout(resolve, 200))
  }
  throw new Error(`Workerd did not become ready within ${timeoutMs}ms`)
}

async function runUnitTests(): Promise<number> {
  console.log('📋 Running unit tests...')
  console.log('')

  const proc = spawn({
    cmd: [
      'bun',
      'test',
      path.join(__dirname, 'bun.test.ts'),
      path.join(__dirname, 'sqlite.test.ts'),
    ],
    stdio: ['inherit', 'inherit', 'inherit'],
  })

  return proc.exited
}

async function runIntegrationTests(workerdRunning: boolean): Promise<number> {
  console.log('📋 Running integration tests...')
  console.log('')

  const env = { ...process.env }
  if (workerdRunning) {
    env.WORKERD_RUNNING = '1'
  }

  const proc = spawn({
    cmd: ['bun', 'test', path.join(__dirname, 'bun-worker.test.ts')],
    env,
    stdio: ['inherit', 'inherit', 'inherit'],
  })

  return proc.exited
}

async function main() {
  console.log('🚀 Bun Compatibility Layer Test Runner')
  console.log('============================================================')
  console.log('')

  let workerdProcess: Subprocess | null = null
  let exitCode = 0

  try {
    // Check if build is complete
    const bundlePath = path.resolve(__dirname, '../../dist/bun/bun-bundle.js')
    const bundleExists = await Bun.file(bundlePath).exists()

    if (!bundleExists) {
      console.log('⚠️  Bundle not found. Building...')
      const buildProc = spawn({
        cmd: ['bun', 'run', 'build:bun'],
        cwd: path.resolve(__dirname, '../..'),
        stdio: ['inherit', 'inherit', 'inherit'],
      })
      const buildExit = await buildProc.exited
      if (buildExit !== 0) {
        console.error('❌ Build failed')
        process.exit(1)
      }
      console.log('')
    }

    // Run unit tests first
    console.log('============================================================')
    console.log('PHASE 1: Unit Tests')
    console.log('============================================================')
    console.log('')

    const unitExit = await runUnitTests()
    if (unitExit !== 0) {
      console.error('')
      console.error('❌ Unit tests failed')
      exitCode = 1
    } else {
      console.log('')
      console.log('✅ Unit tests passed')
    }

    console.log('')
    console.log('============================================================')
    console.log('PHASE 2: Integration Tests')
    console.log('============================================================')
    console.log('')

    // Check if workerd is already running
    const alreadyRunning = await checkWorkerdRunning()

    if (alreadyRunning) {
      console.log('ℹ️  Workerd already running at', WORKERD_URL)
    } else {
      // Start workerd
      console.log('🏃 Starting workerd...')
      console.log(`   Config: ${WORKERD_CONFIG}`)

      workerdProcess = spawn({
        cmd: ['workerd', 'serve', '--experimental', WORKERD_CONFIG],
        stdout: 'pipe',
        stderr: 'pipe',
      })

      console.log(`   PID: ${workerdProcess.pid}`)

      // Wait for workerd to be ready
      console.log('⏳ Waiting for workerd to be ready...')
      await waitForWorkerd(STARTUP_TIMEOUT)
      console.log('   ✅ Workerd is ready')
    }

    console.log('')

    // Test basic connectivity
    console.log('📦 Testing basic connectivity...')
    const start = Date.now()
    const response = await fetch(`${WORKERD_URL}/`)
    const data = (await response.json()) as {
      message: string
      bunVersion: string
    }

    if (response.status === 200 && data.message.includes('Bun')) {
      console.log(
        `   ✅ Basic connectivity test passed (${Date.now() - start}ms)`,
      )
      console.log(`   Bun version: ${data.bunVersion}`)
    } else {
      throw new Error('Basic connectivity test failed')
    }

    console.log('')

    // Run integration tests
    const intExit = await runIntegrationTests(true)
    if (intExit !== 0) {
      console.error('')
      console.error('❌ Integration tests failed')
      exitCode = 1
    } else {
      console.log('')
      console.log('✅ Integration tests passed')
    }
  } catch (error) {
    console.error('')
    console.error('❌ Test run failed:', error)
    exitCode = 1
  } finally {
    // Cleanup
    if (workerdProcess) {
      console.log('')
      console.log('🧹 Cleaning up...')
      workerdProcess.kill()
      await workerdProcess.exited
      console.log('   ✅ Workerd stopped')
    }
  }

  console.log('')
  console.log('============================================================')
  if (exitCode === 0) {
    console.log('✅ All tests passed')
  } else {
    console.log('❌ Some tests failed')
  }
  console.log('============================================================')

  process.exit(exitCode)
}

main()
