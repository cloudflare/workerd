#!/usr/bin/env node
// Minimal stdio MCP server exposing the tools in this directory to Claude Code, which registers
// it in `/.mcp.json`. opencode loads the same tools natively from `.opencode/tools/`.
//
// Speaks newline-delimited JSON-RPC 2.0 and implements only what a tools-only server needs:
// `initialize`, `ping`, `tools/list`, and `tools/call`. It has no dependencies, so it runs on a
// fresh checkout with Node 22.18 or later (for TypeScript type stripping).

import path from 'node:path';
import { createInterface } from 'node:readline';
import { compatDateAt } from './compat-date-at.ts';
import { nextCapnpOrdinal } from './next-capnp-ordinal.ts';
import { toJsonSchema, type ToolDef } from './tool-def.ts';

const TOOLS: ToolDef<any>[] = [compatDateAt, nextCapnpOrdinal];
const ROOT = path.resolve(import.meta.dirname, '../..');

type Id = string | number | null;

function send(message: object): void {
  process.stdout.write(JSON.stringify({ jsonrpc: '2.0', ...message }) + '\n');
}

async function handle(method: string, params: any): Promise<object> {
  switch (method) {
    case 'initialize':
      return {
        protocolVersion: params?.protocolVersion ?? '2025-06-18',
        capabilities: { tools: {} },
        serverInfo: { name: 'workerd', version: '1.0.0' },
      };
    case 'ping':
      return {};
    case 'tools/list':
      return {
        tools: TOOLS.map((def) => ({
          name: def.name,
          description: def.description,
          inputSchema: toJsonSchema(def),
        })),
      };
    case 'tools/call': {
      const def = TOOLS.find((t) => t.name === params?.name);
      if (!def) throw new RpcError(-32602, `Unknown tool: ${params?.name}`);
      const args = params.arguments ?? {};
      const problem = checkArgs(def, args);
      if (problem) {
        return { content: [{ type: 'text', text: problem }], isError: true };
      }
      try {
        const text = await def.run(ROOT, args);
        return { content: [{ type: 'text', text }] };
      } catch (e: any) {
        return {
          content: [{ type: 'text', text: String(e?.stack ?? e) }],
          isError: true,
        };
      }
    }
    default:
      throw new RpcError(-32601, `Method not found: ${method}`);
  }
}

// Checks `args` against the tool's declared arguments, returning a message for the model if they
// don't match. opencode gets the same check from the zod schema built by `toOpencodeTool`.
function checkArgs(def: ToolDef<any>, args: unknown): string | undefined {
  if (typeof args !== 'object' || args === null || Array.isArray(args)) {
    return `Error: arguments to ${def.name} must be an object.`;
  }
  const values = args as Record<string, unknown>;
  for (const [name, arg] of Object.entries(def.args)) {
    const value = values[name];
    if (value === undefined) {
      if (!arg.optional) {
        return `Error: ${def.name} requires the '${name}' argument.`;
      }
    } else if (typeof value !== 'string') {
      return `Error: the '${name}' argument to ${def.name} must be a string.`;
    }
  }
  return undefined;
}

class RpcError extends Error {
  code: number;
  constructor(code: number, message: string) {
    super(message);
    this.code = code;
  }
}

async function dispatch(line: string): Promise<void> {
  let id: Id = null;
  try {
    const request = JSON.parse(line);
    // Notifications (no id) need no response; this server ignores them all.
    if (request.id === undefined) return;
    id = request.id;
    send({ id, result: await handle(request.method, request.params) });
  } catch (e: any) {
    const code =
      e instanceof RpcError
        ? e.code
        : e instanceof SyntaxError
          ? -32700
          : -32603;
    send({ id, error: { code, message: String(e?.message ?? e) } });
  }
}

for await (const line of createInterface({ input: process.stdin })) {
  if (line.trim()) void dispatch(line);
}
