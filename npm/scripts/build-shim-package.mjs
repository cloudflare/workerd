// Adapted from evanw/esbuild

import path from 'path';
import fs from 'fs';

function buildNeutralLib() {
  const pjPath = path.join('npm', 'workerd', 'package.json');
  const package_json = JSON.parse(fs.readFileSync(pjPath, 'utf8'));
  package_json.optionalDependencies = {
    '@cloudflare/workerd-darwin-arm64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-darwin-64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-linux-arm64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-linux-64': process.env.WORKERD_VERSION
  };
  fs.writeFileSync(pjPath, JSON.stringify(package_json, null, 2) + '\n');
}

buildNeutralLib();
