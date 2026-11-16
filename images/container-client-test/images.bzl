"""The container tests' images."""

# The image tarballs the container tests load. They need the repositories that workerd's
# MODULE.bazel declares for them (rules_oci and the images it pulls), so a build that loads workerd
# as a plain repository, without that file, leaves //build/config:container_test_images off.
CONTAINER_TEST_IMAGES = select({
    Label("//build/config:container_test_images_enabled"): [
        Label("//images/container-client-test:proxy-everything-tarball"),
        Label("//images/container-client-test:tarball"),
    ],
    "//conditions:default": [],
})
