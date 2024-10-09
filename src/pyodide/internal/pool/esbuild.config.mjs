import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';

const pyodideRootDir = dirname(
  dirname(dirname(new URL(import.meta.url).pathname))
);

let resolvePlugin = {
  name: 'example',
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
