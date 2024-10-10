import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';
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
        path = join(pyodideRootDir, rest);
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
