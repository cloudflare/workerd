# Async Trace JSON Format Specification

This document describes the JSON format used by the workerd async tracing system and consumed by the async-trace-viewer visualization tool.

## Overview

The trace captures async operation tracking similar to Node.js's async_hooks, enabling bubbleprof-style visualization of async activity within a Worker request.

## JSON Schema

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "AsyncTrace",
  "description": "Async trace data for a single Worker request",
  "type": "object",
  "required": ["requestDurationNs", "resources", "stackTraces", "annotations"],
  "properties": {
    "requestDurationNs": {
      "type": "integer",
      "description": "Total request duration in nanoseconds from start to completion",
      "minimum": 0
    },
    "resources": {
      "type": "array",
      "description": "List of async resources created during the request",
      "items": { "$ref": "#/$defs/Resource" }
    },
    "stackTraces": {
      "type": "array",
      "description": "Deduplicated stack traces referenced by resources",
      "items": { "$ref": "#/$defs/StackTrace" }
    },
    "annotations": {
      "type": "array",
      "description": "Key-value annotations attached to specific resources",
      "items": { "$ref": "#/$defs/Annotation" }
    }
  },
  "$defs": {
    "Resource": {
      "type": "object",
      "required": ["asyncId", "triggerId", "type", "stackTraceId", "createdAt", "callbackStartedAt", "callbackEndedAt", "destroyedAt"],
      "properties": {
        "asyncId": {
          "type": "integer",
          "description": "Unique identifier for this async resource",
          "minimum": 1
        },
        "triggerId": {
          "type": "integer",
          "description": "asyncId of the resource that triggered this one (0 = root/none)",
          "minimum": 0
        },
        "type": {
          "type": "string",
          "description": "Resource type identifier",
          "enum": [
            "root",
            "js-promise",
            "kj-promise",
            "kj-to-js",
            "js-to-kj",
            "fetch",
            "cache-get",
            "cache-put",
            "kv-get",
            "kv-put",
            "kv-delete",
            "kv-list",
            "do-get",
            "do-put",
            "do-delete",
            "do-list",
            "do-call",
            "r2-get",
            "r2-put",
            "r2-delete",
            "r2-list",
            "d1-query",
            "queue-send",
            "timer",
            "stream-read",
            "stream-write",
            "stream-pipe-to",
            "stream-pipe-through",
            "websocket",
            "crypto",
            "ai-inference",
            "other"
          ]
        },
        "stackTraceId": {
          "type": "integer",
          "description": "Reference to stackTraces array by id field",
          "minimum": 0
        },
        "createdAt": {
          "type": "integer",
          "description": "Nanoseconds from request start when resource was created",
          "minimum": 0
        },
        "callbackStartedAt": {
          "type": "integer",
          "description": "Nanoseconds from request start when callback began (0 = never ran)",
          "minimum": 0
        },
        "callbackEndedAt": {
          "type": "integer",
          "description": "Nanoseconds from request start when callback finished (0 = never finished)",
          "minimum": 0
        },
        "destroyedAt": {
          "type": "integer",
          "description": "Nanoseconds from request start when resource was destroyed (0 = not yet destroyed)",
          "minimum": 0
        }
      }
    },
    "StackTrace": {
      "type": "object",
      "required": ["id", "frames"],
      "properties": {
        "id": {
          "type": "integer",
          "description": "Unique identifier referenced by resources",
          "minimum": 0
        },
        "frames": {
          "type": "array",
          "description": "Stack frames from innermost to outermost",
          "items": {
            "type": "string",
            "description": "Frame in format: functionName @ script:line:column"
          }
        }
      }
    },
    "Annotation": {
      "type": "object",
      "required": ["asyncId", "key", "value"],
      "properties": {
        "asyncId": {
          "type": "integer",
          "description": "The resource this annotation is attached to",
          "minimum": 1
        },
        "key": {
          "type": "string",
          "description": "Annotation key (e.g., 'url', 'method', 'delay')"
        },
        "value": {
          "type": "string",
          "description": "Annotation value"
        }
      }
    }
  }
}
```

## Resource Types

### Core Types

| Type | Description |
|------|-------------|
| `root` | The root context (request handler entry point) |
| `js-promise` | JavaScript Promise |
| `kj-promise` | KJ promise (C++ side) |
| `kj-to-js` | KJ promise wrapped for JavaScript consumption |
| `js-to-kj` | JavaScript promise being awaited in KJ/C++ |

### API Operations

| Type | Description |
|------|-------------|
| `fetch` | `fetch()` subrequest |
| `cache-get` | Cache API get operation |
| `cache-put` | Cache API put operation |
| `timer` | `setTimeout` or `setInterval` |

### Storage Operations

| Type | Description |
|------|-------------|
| `kv-get` | KV namespace get |
| `kv-put` | KV namespace put |
| `kv-delete` | KV namespace delete |
| `kv-list` | KV namespace list |
| `do-get` | Durable Object storage get |
| `do-put` | Durable Object storage put |
| `do-delete` | Durable Object storage delete |
| `do-list` | Durable Object storage list |
| `do-call` | Durable Object RPC call |
| `r2-get` | R2 bucket get |
| `r2-put` | R2 bucket put |
| `r2-delete` | R2 bucket delete |
| `r2-list` | R2 bucket list |
| `d1-query` | D1 database query |
| `queue-send` | Queue send operation |

### Stream Operations

| Type | Description |
|------|-------------|
| `stream-read` | ReadableStream read operation |
| `stream-write` | WritableStream write operation |
| `stream-pipe-to` | `ReadableStream.pipeTo()` |
| `stream-pipe-through` | `ReadableStream.pipeThrough()` |

### Other Operations

| Type | Description |
|------|-------------|
| `websocket` | WebSocket operation |
| `crypto` | Async crypto operation |
| `ai-inference` | AI inference operation |
| `other` | Unclassified operation |

## Timing Model

All timing values are in **nanoseconds** relative to request start (when `requestDurationNs` timer began).

```
createdAt ─────────────────────────────────────────────────────────────────►
           │
           │  (async wait)
           │
           ▼
callbackStartedAt ─────────────────────────────────────────────────────────►
           │
           │  (sync execution)
           │
           ▼
callbackEndedAt ───────────────────────────────────────────────────────────►
           │
           │  (cleanup/waiting for GC)
           │
           ▼
destroyedAt ───────────────────────────────────────────────────────────────►
```

### Computed Metrics

- **Async delay**: `callbackStartedAt - createdAt` (time waiting for async operation)
- **Sync time**: `callbackEndedAt - callbackStartedAt` (time executing callback)
- **Total time**: `callbackEndedAt - createdAt` (creation to callback completion)

### Zero Values

A value of `0` for timing fields indicates:
- `callbackStartedAt = 0`: Callback never ran (promise never resolved, timer cancelled, etc.)
- `callbackEndedAt = 0`: Callback never finished (still running or never started)
- `destroyedAt = 0`: Resource not yet destroyed (may still be referenced)

## Common Annotation Keys

| Key | Used By | Description |
|-----|---------|-------------|
| `url` | `fetch` | The URL being fetched |
| `method` | `fetch` | HTTP method (GET, POST, etc.) |
| `delay` | `timer` | Timer delay in milliseconds |
| `type` | `timer` | Timer type (`setTimeout` or `setInterval`) |

## Example

```json
{
  "requestDurationNs": 17352613,
  "resources": [
    {
      "asyncId": 1,
      "triggerId": 0,
      "type": "root",
      "stackTraceId": 0,
      "createdAt": 0,
      "callbackStartedAt": 0,
      "callbackEndedAt": 17312797,
      "destroyedAt": 17313045
    },
    {
      "asyncId": 2,
      "triggerId": 1,
      "type": "js-promise",
      "stackTraceId": 1,
      "createdAt": 3309095,
      "callbackStartedAt": 10582028,
      "callbackEndedAt": 11644945,
      "destroyedAt": 0
    },
    {
      "asyncId": 3,
      "triggerId": 1,
      "type": "timer",
      "stackTraceId": 2,
      "createdAt": 3888952,
      "callbackStartedAt": 0,
      "callbackEndedAt": 0,
      "destroyedAt": 0
    }
  ],
  "stackTraces": [
    {"id": 0, "frames": ["fetch @ worker:2:14"]},
    {"id": 1, "frames": ["fetch @ worker:4:27"]},
    {"id": 2, "frames": ["result1 @ worker:5:7", "fetch @ worker:4:27"]}
  ],
  "annotations": [
    {"asyncId": 3, "key": "delay", "value": "10"},
    {"asyncId": 3, "key": "type", "value": "setTimeout"}
  ]
}
```

## Causality Graph

Resources form a directed acyclic graph (DAG) through their `triggerId` relationships:

- The `root` resource (asyncId=1) has `triggerId=0` (no parent)
- All other resources have `triggerId` pointing to the resource whose callback was executing when they were created
- This creates a causality chain showing which async operation led to which

## Generating Traces

Traces are generated by workerd when running with verbose logging and Perfetto tracing enabled:

```bash
bazel-bin/src/workerd/server/workerd serve \
    --verbose \
    --perfetto-trace=/tmp/trace.perfetto=workerd \
    your-config.capnp
```

The trace JSON is output to stderr with prefix:
```
AsyncTrace completed; toJson() = {...}
```

## Visualization

Load traces into the visualization tool at `tools/async-trace-viewer/index.html`:

1. Start a local server: `python3 -m http.server 8888`
2. Open `http://localhost:8888`
3. Load trace via file picker, paste, or demo dropdown
