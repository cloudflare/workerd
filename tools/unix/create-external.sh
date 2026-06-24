#!/bin/sh

# Create a symlink to Bazel's external/ directory. This simplifies the
# paths provided to clangd in compile_flags.txt for language server
# use.

output_path=$(bazel info output_path)
workspace=$(bazel info workspace)
# Note: -n (--no-dereference) is required so that an existing "external"
# symlink pointing at a directory is replaced rather than dereferenced (which
# would create the new link *inside* the target directory). Both GNU and BSD
# ln support -n. The previously used -F flag is a no-op for this case on Linux.
external="${workspace}/external"
ln -sfn "${output_path}/../../../external" "${external}"

# Temporary warning that compile_commands.json exists and will
# interfere with the intended clangd setup.
compile_commands="${workspace}/compile_commands.json"
if [ -f "compile_commands.json" ] ; then
  cat<<COMPILE_COMMANDS_WARNING
WARNING: This workspace has a compile_commands.json file, but workerd
has moved to using compile_flags.txt for clangd instead. To improve
code completion and navigation in your editor, consider running:

  rm "${compile_commands}"

COMPILE_COMMANDS_WARNING
fi
