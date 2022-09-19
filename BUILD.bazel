load("//:build/wd_cc_embed.bzl", "wd_cc_embed")

wd_cc_embed(
    name = "icudata-embed",
    base_name = "icudata-embed",
    strip_include_prefix = "",
    defines = [
        "WORKERD_ICU_DATA_EMBED=EMBED_com_googlesource_chromium_icu_common_icudtl_dat"
    ],
    data = ["@com_googlesource_chromium_icu//:icudata"],
    visibility = ["//visibility:public"],
)
