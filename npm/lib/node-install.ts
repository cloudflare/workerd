/* Adapted from github.com/evanw/esbuild
MIT License
Copyright (c) 2020 Evan Wallace
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/
import {
  downloadedBinPath,
  pkgAndSubpathForCurrentPlatform
} from './node-platform';

import fs from 'fs';
import os from 'os';
import path from 'path';
import zlib from 'zlib';
import https from 'https';
import child_process from 'child_process';

declare const LATEST_COMPATIBILITY_DATE: string;

// Make something semver-ish
const WORKERD_VERSION = `1.${LATEST_COMPATIBILITY_DATE.split('-').join('')}.0`;

const toPath = path.join(__dirname, 'bin', 'workerd');
let isToPathJS = true;

function validateBinaryVersion(...command: string[]): void {
  command.push('--version');
  const stdout = child_process
    .execFileSync(command.shift()!, command, {
      // Without this, this install script strangely crashes with the error
      // "EACCES: permission denied, write" but only on Ubuntu Linux when node is
      // installed from the Snap Store. This is not a problem when you download
      // the official version of node. The problem appears to be that stderr
      // (i.e. file descriptor 2) isn't writable?
      //
      // More info:
      // - https://snapcraft.io/ (what the Snap Store is)
      // - https://nodejs.org/dist/ (download the official version of node)
      // - https://github.com/evanw/esbuild/issues/1711#issuecomment-1027554035
      //
      stdio: 'pipe'
    })
    .toString()
    .trim();
  if (stdout !== LATEST_COMPATIBILITY_DATE) {
    throw new Error(
      `Expected ${JSON.stringify(
        LATEST_COMPATIBILITY_DATE
      )} but got ${JSON.stringify(stdout)}`
    );
  }
}

function isYarn(): boolean {
  const { npm_config_user_agent } = process.env;
  if (npm_config_user_agent) {
    return /\byarn\//.test(npm_config_user_agent);
  }
  return false;
}

function fetch(url: string): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    https
      .get(url, (res) => {
        if (
          (res.statusCode === 301 || res.statusCode === 302) &&
          res.headers.location
        )
          return fetch(res.headers.location).then(resolve, reject);
        if (res.statusCode !== 200)
          return reject(new Error(`Server responded with ${res.statusCode}`));
        let chunks: Buffer[] = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => resolve(Buffer.concat(chunks)));
      })
      .on('error', reject);
  });
}

function extractFileFromTarGzip(buffer: Buffer, subpath: string): Buffer {
  try {
    buffer = zlib.unzipSync(buffer);
  } catch (err: any) {
    throw new Error(
      `Invalid gzip data in archive: ${(err && err.message) || err}`
    );
  }
  let str = (i: number, n: number) =>
    String.fromCharCode(...buffer.subarray(i, i + n)).replace(/\0.*$/, '');
  let offset = 0;
  subpath = `package/${subpath}`;
  while (offset < buffer.length) {
    let name = str(offset, 100);
    let size = parseInt(str(offset + 124, 12), 8);
    offset += 512;
    if (!isNaN(size)) {
      if (name === subpath) return buffer.subarray(offset, offset + size);
      offset += (size + 511) & ~511;
    }
  }
  throw new Error(`Could not find ${JSON.stringify(subpath)} in archive`);
}

function installUsingNPM(pkg: string, subpath: string, binPath: string): void {
  // Erase "npm_config_global" so that "npm install --global workerd" works.
  // Otherwise this nested "npm install" will also be global, and the install
  // will deadlock waiting for the global installation lock.
  const env = { ...process.env, npm_config_global: undefined };

  // Create a temporary directory inside the "workerd" package with an empty
  // "package.json" file. We'll use this to run "npm install" in.
  const libDir = path.dirname(require.resolve('workerd'));
  const installDir = path.join(libDir, 'npm-install');
  fs.mkdirSync(installDir);
  try {
    fs.writeFileSync(path.join(installDir, 'package.json'), '{}');

    // Run "npm install" in the temporary directory which should download the
    // desired package. Try to avoid unnecessary log output. This uses the "npm"
    // command instead of a HTTP request so that it hopefully works in situations
    // where HTTP requests are blocked but the "npm" command still works due to,
    // for example, a custom configured npm registry and special firewall rules.
    child_process.execSync(
      `npm install --loglevel=error --prefer-offline --no-audit --progress=false ${pkg}@${WORKERD_VERSION}`,
      { cwd: installDir, stdio: 'pipe', env }
    );

    // Move the downloaded binary executable into place. The destination path
    // is the same one that the JavaScript API code uses so it will be able to
    // find the binary executable here later.
    const installedBinPath = path.join(
      installDir,
      'node_modules',
      pkg,
      subpath
    );
    fs.renameSync(installedBinPath, binPath);
  } finally {
    // Try to clean up afterward so we don't unnecessarily waste file system
    // space. Leaving nested "node_modules" directories can also be problematic
    // for certain tools that scan over the file tree and expect it to have a
    // certain structure.
    try {
      removeRecursive(installDir);
    } catch {
      // Removing a file or directory can randomly break on Windows, returning
      // EBUSY for an arbitrary length of time. I think this happens when some
      // other program has that file or directory open (e.g. an anti-virus
      // program). This is fine on Unix because the OS just unlinks the entry
      // but keeps the reference around until it's unused. There's nothing we
      // can do in this case so we just leave the directory there.
    }
  }
}

function removeRecursive(dir: string): void {
  for (const entry of fs.readdirSync(dir)) {
    const entryPath = path.join(dir, entry);
    let stats;
    try {
      stats = fs.lstatSync(entryPath);
    } catch {
      continue; // Guard against https://github.com/nodejs/node/issues/4760
    }
    if (stats.isDirectory()) removeRecursive(entryPath);
    else fs.unlinkSync(entryPath);
  }
  fs.rmdirSync(dir);
}

function maybeOptimizePackage(binPath: string): void {
  // This package contains a "bin/workerd" JavaScript file that finds and runs
  // the appropriate binary executable. However, this means that running the
  // "workerd" command runs another instance of "node" which is way slower than
  // just running the binary executable directly.
  //
  // Here we optimize for this by replacing the JavaScript file with the binary
  // executable at install time.
  //
  // This doesn't work with Yarn both because of lack of support for binary
  // files in Yarn 2+ (see https://github.com/yarnpkg/berry/issues/882) and
  // because Yarn (even Yarn 1?) may run the same install scripts in the same
  // place multiple times from different platforms, especially when people use
  // Docker. Avoid idempotency issues by just not optimizing when using Yarn.
  //
  // This optimization also doesn't apply when npm's "--ignore-scripts" flag is
  // used since in that case this install script will not be run.
  if (!isYarn()) {
    const tempPath = path.join(__dirname, 'bin-workerd');
    try {
      // First link the binary with a temporary file. If this fails and throws an
      // error, then we'll just end up doing nothing. This uses a hard link to
      // avoid taking up additional space on the file system.
      fs.linkSync(binPath, tempPath);

      // Then use rename to atomically replace the target file with the temporary
      // file. If this fails and throws an error, then we'll just end up leaving
      // the temporary file there, which is harmless.
      fs.renameSync(tempPath, toPath);

      // If we get here, then we know that the target location is now a binary
      // executable instead of a JavaScript file.
      isToPathJS = false;

      // If this install script is being re-run, then "renameSync" will fail
      // since the underlying inode is the same (it just returns without doing
      // anything, and without throwing an error). In that case we should remove
      // the file manually.
      fs.unlinkSync(tempPath);
    } catch {
      // Ignore errors here since this optimization is optional
    }
  }
}

async function downloadDirectlyFromNPM(
  pkg: string,
  subpath: string,
  binPath: string
): Promise<void> {
  // If that fails, the user could have npm configured incorrectly or could not
  // have npm installed. Try downloading directly from npm as a last resort.
  const url = `https://registry.npmjs.org/${pkg}/-/${pkg}-${WORKERD_VERSION}.tgz`;
  console.error(`[workerd] Trying to download ${JSON.stringify(url)}`);
  try {
    fs.writeFileSync(
      binPath,
      extractFileFromTarGzip(await fetch(url), subpath)
    );
    fs.chmodSync(binPath, 0o755);
  } catch (e: any) {
    console.error(
      `[workerd] Failed to download ${JSON.stringify(url)}: ${
        (e && e.message) || e
      }`
    );
    throw e;
  }
}

async function checkAndPreparePackage(): Promise<void> {
  const { pkg, subpath } = pkgAndSubpathForCurrentPlatform();

  let binPath: string;
  try {
    // First check for the binary package from our "optionalDependencies". This
    // package should have been installed alongside this package at install time.
    binPath = require.resolve(`${pkg}/${subpath}`);
  } catch (e) {
    console.error(`[workerd] Failed to find package "${pkg}" on the file system

This can happen if you use the "--no-optional" flag. The "optionalDependencies"
package.json feature is used by workerd to install the correct binary executable
for your current platform. This install script will now attempt to work around
this. If that fails, you need to remove the "--no-optional" flag to use workerd.
`);

    // If that didn't work, then someone probably installed workerd with the
    // "--no-optional" flag. Attempt to compensate for this by downloading the
    // package using a nested call to "npm" instead.
    //
    // THIS MAY NOT WORK. Package installation uses "optionalDependencies" for
    // a reason: manually downloading the package has a lot of obscure edge
    // cases that fail because people have customized their environment in
    // some strange way that breaks downloading. This code path is just here
    // to be helpful but it's not the supported way of installing workerd.
    binPath = downloadedBinPath(pkg, subpath);
    try {
      console.error(`[workerd] Trying to install package "${pkg}" using npm`);
      installUsingNPM(pkg, subpath, binPath);
    } catch (e2: any) {
      console.error(
        `[workerd] Failed to install package "${pkg}" using npm: ${
          (e2 && e2.message) || e2
        }`
      );

      // If that didn't also work, then something is likely wrong with the "npm"
      // command. Attempt to compensate for this by manually downloading the
      // package from the npm registry over HTTP as a last resort.
      try {
        await downloadDirectlyFromNPM(pkg, subpath, binPath);
      } catch (e3: any) {
        throw new Error(`Failed to install package "${pkg}"`);
      }
    }
  }

  maybeOptimizePackage(binPath);
}

checkAndPreparePackage().then(() => {
  if (isToPathJS) {
    // We need "node" before this command since it's a JavaScript file
    validateBinaryVersion(process.execPath, toPath);
  } else {
    // This is no longer a JavaScript file so don't run it using "node"
    validateBinaryVersion(toPath);
  }
});
