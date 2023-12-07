# Pyodide

This folder includes the code needed to generate the Pyodide bundle which we
build into workerd. There is also `build/BUILD.pyodide` (which just exposes the
files from the Pyodide release as public to workerd) and
`workerd/api/pyodide/pyodide.h` which adds the bundle to the module registry if
the appropriate flags are set.

It requires the `workerd-autogate-builtin-wasm-modules` autogate flag and the
`experimental` compatibility flag to enable.

See `samples/pyodide/` for an example using this in its current state.

## Do we really want to be compiling this stuff into workerd?

Updating Pyodide's major version carries significant risk of breaking scripts.
Currently each Pyodide version comes with a specific Python version and a
specific lockfile of packages. Any script that breaks on newest Python or with a
newer version of a package will break on upgrade.

Thus, we have to let people pin older versions of Pyodide. Before we start
supporting multiple Pyodide, we'll want to be able to dynamically fetch the
appropriate Pyodide version at runtime rather than bloating binary size by
including lots of potentially unused code. Similarly, we will likely want to
dynamically fetch the Python packages people use.

The present approach is just the fastest way to get something working.

## What's happening here?

Pyodide's distribution consists of:
1. The main "emscripten binary" which is `pyodide.asm.js` and `pyodide.asm.wasm`
2. A loader `pyodide.js`
3. The Python + Pyodide stdlib `python_stdlib.zip`
4. A package lockfile `pyodide-lock.json` containing version and dependency
   information for all packages that were built as part of the release.
5. ... other stuff

We are currently taking just `pyodide.asm.js`, `pyodide.asm.wasm`, and
`python_stdlib.zip`. We disable package loading entirely by not setting up the
lock file. We'll eventually want our own mechanism for setting up packages that
probably happens at configuration time. The file `src/pyodide/python.js` serves
as a simplified substitute for `pyodide.js` which drops the package loader and
most other config, but adds support for memory snapshots. We want to upstream
the memory snapshot support into Pyodide, but having our own reduced loader will
probably continue to be beneficial in the future.

To reduce memory usage and startup time, we'll also want to link our own version
of the Emscripten binary `pyodide.asm.js` and `pyodide.asm.wasm`. Due to
limitations in Emscripten's dynamic linking, they are bloated by statically
linked junk that is rarely used. My hope is to include all the static libraries
that go into Pyodide in a release artifact for the next Pyodide version. Then as
part of our build process we can do a modified final link step that drops stuff
we don't need.
