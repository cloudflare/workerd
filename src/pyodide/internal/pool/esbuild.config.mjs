import { dirname, join } from 'node:path';
import { existsSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'url';

const pyodideRootDir = dirname(
  dirname(dirname(fileURLToPath(import.meta.url)))
);

let resolvePlugin = {
  name: 'pyodide-internal',
  setup(build) {
    // Redirect all paths starting with "images/" to "./public/images/"
    build.onResolve({ filter: /pyodide-internal:.*/ }, (args) => {
      let rest = args.path.split(':')[1];
      let path;
      if (rest.startsWith('generated')) {
        // I couldn't figure out how to pass down the version, so instead we'll look through the
        // directories in `pyodideRootDir` and find one that starts with a 0. This will work until
        // Pyodide has a 1.0 release.
        const dir = readdirSync(pyodideRootDir).filter((x) =>
          x.startsWith('0')
        )[0];
        path = join(pyodideRootDir, dir, rest);
        if (!existsSync(path)) {
          path += '.js';
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
};
