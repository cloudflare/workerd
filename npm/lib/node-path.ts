#!/usr/bin/env node

import { generateBinPath } from './node-platform';
const { binPath } = generateBinPath();

export default binPath;
