"""Rules for generating doxygen documentation."""

def _doxygen_docs_impl(ctx):
    """Generates doxygen documentation."""

    # Create output directory
    output_dir = ctx.actions.declare_directory(ctx.attr.name + "_html")

    # Get the doxygen binary
    doxygen_files = ctx.attr.doxygen[DefaultInfo].files.to_list()
    if not doxygen_files:
        fail("No doxygen binary found")
    doxygen_bin = doxygen_files[0]

    # Create a script to run doxygen
    script = ctx.actions.declare_file(ctx.attr.name + "_run.sh")

    script_content = """#!/bin/bash
set -e

DOXYGEN="{doxygen}"
DOXYFILE="{doxyfile}"
OUTPUT_DIR="{output_dir}"
WORKSPACE="{workspace}"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Run doxygen from workspace root
cd "$WORKSPACE"
"$DOXYGEN" "$DOXYFILE"

# Move output to bazel output location
if [ -d "docs/api/html" ]; then
    cp -r docs/api/html/* "$OUTPUT_DIR/"
fi
""".format(
        doxygen = doxygen_bin.path,
        doxyfile = ctx.file.doxyfile.path,
        output_dir = output_dir.path,
        workspace = ctx.file.doxyfile.dirname,
    )

    ctx.actions.write(
        output = script,
        content = script_content,
        is_executable = True,
    )

    # Collect all source files
    srcs = []
    for src in ctx.attr.srcs:
        srcs.extend(src[DefaultInfo].files.to_list())

    ctx.actions.run(
        executable = script,
        inputs = [ctx.file.doxyfile, doxygen_bin] + srcs,
        outputs = [output_dir],
        mnemonic = "Doxygen",
        progress_message = "Generating doxygen documentation",
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([output_dir]))]

doxygen_docs = rule(
    implementation = _doxygen_docs_impl,
    attrs = {
        "doxyfile": attr.label(
            mandatory = True,
            allow_single_file = True,
            doc = "The Doxyfile configuration",
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Source files to document",
        ),
        "doxygen": attr.label(
            default = "//build/doxygen",
            doc = "The doxygen binary",
        ),
    },
    doc = "Generates doxygen documentation from source files",
)
