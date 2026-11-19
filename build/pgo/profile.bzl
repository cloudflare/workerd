"""The `@workerd_pgo_profile` repository, which holds the PGO profile (see build/pgo/README.md)."""

def _profile_repository_impl(rctx):
    # The URL names a single compressed file, such as workerd-<date>-<commit>.profdata.zst, which
    # Bazel decompresses to the same name without the .zst. It's renamed to the name the build
    # refers to.
    name = rctx.attr.url.rsplit("/", 1)[1].removesuffix(".zst")
    rctx.download_and_extract(url = rctx.attr.url, integrity = rctx.attr.integrity)
    rctx.rename(name, "workerd.profdata")
    rctx.file("BUILD.bazel", 'exports_files(["workerd.profdata"])\n')

    # The integrity check pins the contents.
    return rctx.repo_metadata(reproducible = True)

_profile_repository = repository_rule(
    implementation = _profile_repository_impl,
    attrs = {
        "integrity": attr.string(mandatory = True),
        "url": attr.string(mandatory = True),
    },
)

def _pgo_profile_impl(module_ctx):
    profile = json.decode(module_ctx.read(Label("//build/pgo:profile.json")))
    _profile_repository(
        name = "workerd_pgo_profile",
        integrity = profile["integrity"],
        url = profile["url"],
    )
    return module_ctx.extension_metadata(reproducible = True)

pgo_profile = module_extension(implementation = _pgo_profile_impl)
