// Agent-neutral definitions of the project's custom tools.
//
// Each tool is defined once, as a ToolDef, and exposed to two harnesses:
// - opencode, through the thin wrappers in `.opencode/tools/` (see `toOpencodeTool`);
// - Claude Code, through the MCP server in `mcp-server.ts`, registered in `/.mcp.json`.
//
// Files in this directory must run unmodified under both Bun (opencode) and Node (the MCP
// server), so they use only `node:` modules and erasable TypeScript syntax, and do not import
// `@opencode-ai/plugin`.

export interface ToolArg {
  description: string;
  optional?: boolean;
}

export interface ToolDef<Args extends Record<string, string | undefined>> {
  // Tool name, as registered with the harness.
  name: string;
  description: string;
  // All arguments are strings.
  args: { [K in keyof Args]-?: ToolArg };
  // `root` is the workerd checkout. The result is the text shown to the model; failures are
  // reported as text too, not thrown.
  run(root: string, args: Args): Promise<string>;
}

// The JSON Schema for a tool's arguments, as MCP's `inputSchema` expects.
export function toJsonSchema(def: ToolDef<any>): object {
  const properties: Record<string, object> = {};
  const required: string[] = [];
  for (const [name, arg] of Object.entries<ToolArg>(def.args)) {
    properties[name] = { type: 'string', description: arg.description };
    if (!arg.optional) required.push(name);
  }
  return { type: 'object', properties, required };
}

// Builds an opencode tool from a ToolDef. Takes the plugin's `tool` function as a parameter so
// that this file does not import `@opencode-ai/plugin`, which only opencode installs.
export function toOpencodeTool(tool: any, def: ToolDef<any>): unknown {
  const args: Record<string, unknown> = {};
  for (const [name, arg] of Object.entries<ToolArg>(def.args)) {
    const schema = tool.schema.string();
    args[name] = (arg.optional ? schema.optional() : schema).describe(
      arg.description
    );
  }
  return tool({
    description: def.description,
    args,
    execute: (args: any, ctx: any) =>
      def.run(ctx.worktree || ctx.directory, args),
  });
}
