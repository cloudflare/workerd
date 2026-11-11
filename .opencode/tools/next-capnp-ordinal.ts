// The implementation is shared with Claude Code; see ../lib/tool-def.ts.
import { tool } from '@opencode-ai/plugin';
import { toOpencodeTool } from '../lib/tool-def.ts';
import { nextCapnpOrdinal } from '../lib/next-capnp-ordinal.ts';

export default toOpencodeTool(tool, nextCapnpOrdinal);
