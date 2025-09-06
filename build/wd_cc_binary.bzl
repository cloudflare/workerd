"""wd_cc_binary definition"""

def wd_cc_binary(
        name,
        linkopts = [],
        visibility = None,
        target_compatible_with = [],
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    native.cc_binary(
        name = name,
        # -dead_strip is the macOS equivalent of -ffunction-sections, -Wl,--gc-sections.
        # -no_exported_symbols is used to not include the exports trie, which significantly reduces
        # binary sizes. Unfortunately, the flag and the exports trie are poorly documented. Based
        # on analyzing the binary sections with and without the flag, the information being removed
        # consists of weak binding info, export binding info and stub bindings as described in
        # http://www.newosxbook.com/articles/DYLD.html. The flag itself is described in
        # https://www.wwdcnotes.com/notes/wwdc22/110362/.
        # The affected sections appear to not be needed for debugging and are only used when
        # external code needs to look up bindings in a binary, e.g. when loading a plugin
        # (mac-specific feature). In particular, the symbol table is not affected (the name of the
        # flag is misleading here).
        linkopts = linkopts + select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }) + target_compatible_with,
        visibility = visibility,
        **kwargs
    )

    pkg = native.package_name().removeprefix("src/")
    cross_alias = name + "_cross"
    prebuilt_binary_name = name.removesuffix("_bin")
    native.alias(
        name = cross_alias,
        visibility = visibility,
        actual = select({
            "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/{}/{}.aarch64-linux-gnu".format(pkg, prebuilt_binary_name),
            "//conditions:default": name,
        }),
        # Propagate InstrumentedFilesInfo through the alias for coverage support
        testonly = kwargs.get("testonly", False),
    )
