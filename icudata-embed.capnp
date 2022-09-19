@0x9ed733e2dd15598b;
# This file is a hack to embed the ICU data file which libicu needs at runtime to do its thing.
# It's inconvenient to ship this file separately so we bake it into the binary. (V8's normal GN
# build can actually do this for us, but we use the Bazel build which doesn't have this option.)
#
# We use Cap'n Proto to do the embedding because it's conveniently cross-platform, but this is
# pretty ugly as it generates a 57MB C++ source file (containing a giant byte array literal).
# It would be nice to use some more direct way of embedding binary data into a symbol in the
# executable, but I couldn't figure out how to do this portably. (See the commit which introduced
# this file to see the old Linux-only solution, but it didn't work on Mac and I didn't want to
# figure it out.)

const embeddedIcuDataFile :Data = embed "external/com_googlesource_chromium_icu/common/icudtl.dat";
