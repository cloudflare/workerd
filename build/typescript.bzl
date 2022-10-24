def module_name(ts_name):
    if ts_name.endswith(".ts"):
        return ts_name.removesuffix(".ts")
    if ts_name.endswith(".mts"):
        return ts_name.removesuffix(".mts")
    fail("Expected TypeScript source file, got " + ts_name)

def js_name(ts_name):
    if ts_name.endswith(".ts"):
        return ts_name.removesuffix(".ts") + ".js"
    if ts_name.endswith(".mts"):
        return ts_name.removesuffix(".mts") + ".mjs"
    fail("Expected TypeScript source file, got " + ts_name)
