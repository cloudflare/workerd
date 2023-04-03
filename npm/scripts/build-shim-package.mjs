// Adapted from github.com/evanw/esbuild
// Original copyright and license:
//     Copyright (c) 2020 Evan Wallace
//     MIT License
//
//     Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//     The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
import path from 'path';
import fs from 'fs';

function buildNeutralLib() {
  const pjPath = path.join('npm', 'workerd', 'package.json');
  const package_json = JSON.parse(fs.readFileSync(pjPath, 'utf8'));
  package_json.optionalDependencies = {
    '@cloudflare/workerd-darwin-arm64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-darwin-64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-linux-arm64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-linux-64': process.env.WORKERD_VERSION,
    '@cloudflare/workerd-windows-64': process.env.WORKERD_VERSION
  };
  fs.writeFileSync(pjPath, JSON.stringify(package_json, null, 2) + '\n');
}

buildNeutralLib();
