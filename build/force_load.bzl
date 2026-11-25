"""cc_force_load: forwards a C++ or Rust library, linking its own static libraries whole."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _cc_force_load_impl(ctx):
    dep = ctx.attr.dep
    cc_info = dep[CcInfo]
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    libraries = [
        cc_common.create_library_to_link(
            actions = ctx.actions,
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
            static_library = library.static_library,
            pic_static_library = library.pic_static_library,
            alwayslink = True,
        )
        for linker_input in cc_info.linking_context.linker_inputs.to_list()
        if linker_input.owner == dep.label
        for library in linker_input.libraries
        if library.static_library or library.pic_static_library
    ]
    if not libraries:
        fail("{} has no static libraries of its own to force-load".format(dep.label))
    if not ctx.attr.alwayslink:
        return [cc_info]
    linker_input = cc_common.create_linker_input(owner = ctx.label, libraries = depset(libraries))
    linking_context = cc_common.create_linking_context(linker_inputs = depset([linker_input]))
    return [cc_common.merge_cc_infos(
        direct_cc_infos = [CcInfo(linking_context = linking_context)],
        cc_infos = [cc_info],
    )]

cc_force_load = rule(
    implementation = _cc_force_load_impl,
    doc = """Forwards dep. Where alwayslink is True, binaries also take every object of dep's own
static libraries (-force_load, --whole-archive), as alwayslink on dep itself would, for libraries
whose alwayslink their own rules don't let us set, such as those of other modules and of
crate_universe. The libraries of dep's dependencies, such as the Rust standard library, link as
before.""",
    attrs = {
        "alwayslink": attr.bool(default = True),
        "dep": attr.label(mandatory = True, providers = [CcInfo]),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
