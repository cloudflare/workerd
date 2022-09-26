#!/usr/bin/env node

import { generateBinPath } from "./node-platform";
const { binPath } = generateBinPath();

require("child_process").execFileSync(binPath, process.argv.slice(2), {
  stdio: "inherit",
});
