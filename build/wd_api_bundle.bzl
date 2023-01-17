load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")

CAPNP_TEMPLATE = """@{schema_id};

using Modules = import "/workerd/jsg/modules.capnp";

const {const_name} :Modules.Bundle = (
  modules = [
{modules}
]);
"""

MODULE_TEMPLATE = """(name = "{name}", src = embed "{path}", internal = {internal})"""

def _relative_path(file_path, dir_path):
    if not file_path.startswith(dir_path):
        fail("file_path need to start with dir_path: " + file_path + " vs " + dir_path)
    return file_path.removeprefix(dir_path)

def _gen_api_bundle_capnpn_impl(ctx):
    output_dir = ctx.outputs.out.dirname + "/"

    def _render_module(name, label, internal):
        return MODULE_TEMPLATE.format(
            name = name,
            # capnp doesn't allow ".." dir escape, make paths relative.
            # this won't work for embedding paths outside of rule directory subtree.
            path = _relative_path(
                ctx.expand_location("$(location {})".format(label), ctx.attr.data),
                output_dir,
            ),
            internal = "true" if internal else "false",
        )

    modules = [
        _render_module(ctx.attr.builtin_modules[m], m.label, False)
        for m in ctx.attr.builtin_modules
    ]
    modules += [
        _render_module(ctx.attr.internal_modules[m], m.label, True)
        for m in ctx.attr.internal_modules
    ]

    content = CAPNP_TEMPLATE.format(
        schema_id = ctx.attr.schema_id,
        modules = ",\n".join(modules),
        const_name = ctx.attr.const_name,
    )
    ctx.actions.write(ctx.outputs.out, content)

gen_api_bundle_capnpn = rule(
    implementation = _gen_api_bundle_capnpn_impl,
    attrs = {
        "schema_id": attr.string(mandatory = True),
        "out": attr.output(mandatory = True),
        "builtin_modules": attr.label_keyed_string_dict(allow_files = True),
        "internal_modules": attr.label_keyed_string_dict(allow_files = True),
        "data": attr.label_list(allow_files = True),
        "const_name": attr.string(mandatory = True),
    },
)

def wd_api_bundle(
        name,
        schema_id,
        const_name,
        builtin_modules = {},
        internal_modules = {},
        **kwargs):
    """Generate cc capnp library with api bundle.

    NOTE: Due to capnpc embed limitation all modules must be in the same or sub directory of the
          actual rule usage.

    Args:
     name: cc_capnp_library rule name
     builtin_modules: js src label -> module name dictionary
     internal_modules: js src label -> module name dictionary
     const_name: capnp constant name that will contain bundle definition
     schema_id: capnpn schema id
     **kwargs: rest of cc_capnp_library arguments
    """
    data = list(builtin_modules) + list(internal_modules)

    gen_api_bundle_capnpn(
        name = name + "@gen",
        out = name + ".capnp",
        schema_id = schema_id,
        const_name = const_name,
        builtin_modules = builtin_modules,
        internal_modules = internal_modules,
        data = data,
    )

    cc_capnp_library(
        name = name,
        srcs = [name + ".capnp"],
        strip_include_prefix = "",
        visibility = ["//visibility:public"],
        data = data,
        deps = ["@workerd//src/workerd/jsg:modules_capnp"],
        **kwargs
    )
