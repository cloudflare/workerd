// The implementation is shared with Claude Code; see ../lib/tool-def.ts.
import { tool } from '@opencode-ai/plugin';
import { toOpencodeTool } from '../lib/tool-def.ts';
import { compatDateAt } from '../lib/compat-date-at.ts';

export default toOpencodeTool(tool, compatDateAt);
