import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'url';

const pyodideRootDir = dirname(
  dirname(dirname(fileURLToPath(import.meta.url)))
);

const pyodideVersion = '%PYODIDE_VERSION%';

let resolvePlugin = {
  name: 'pyodide-internal',
  setup(build) {
    // Redirect all paths starting with "images/" to "./public/images/"
    build.onResolve({ filter: /pyodide-internal:.*/ }, (args) => {
      let rest = args.path.split(':')[1];
      let path;
      if (rest.startsWith('generated')) {
        path = join(pyodideRootDir, pyodideVersion, rest);
        if (!existsSync(path)) {
          if (existsSync(path + '.js')) {
            path += '.js';
          } else {
            path += '.mjs';
          }
        }
      } else {
        path = join(pyodideRootDir, 'internal', rest);
        if (!existsSync(path)) {
          path += '.ts';
        }
      }
      return { path };
    });
  },
};

export default {
  plugins: [resolvePlugin],
  logOverride: {
    // Suppress warning about CommonJS 'module' variable in pyodide.asm.js (ES module context)
    // The pyodide.asm.js file contains embedded libraries (like MiniLZ4) that use CommonJS
    // style module.exports checks for compatibility. These are safe to ignore as they won't
    // execute in our ES module context.
    'commonjs-variable-in-esm': 'silent',
  },
};
