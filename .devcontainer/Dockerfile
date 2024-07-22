FROM mcr.microsoft.com/devcontainers/base:jammy

# Install dependencies, including clang via through LLVM APT repository. Note that this
# will also install lldb and clangd alongside dependencies.
ARG LLVM_VERSION=15
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends software-properties-common python3 python3-distutils tclsh \
    && curl -fSsL -o /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && bash /tmp/llvm.sh ${LLVM_VERSION} \
    && apt-get -y install --no-install-recommends libunwind-${LLVM_VERSION} libc++abi1-${LLVM_VERSION} libc++1-${LLVM_VERSION} libc++-${LLVM_VERSION}-dev libclang-rt-${LLVM_VERSION}-dev -o DPkg::options::="--force-overwrite"
ENV PATH /usr/lib/llvm-${LLVM_VERSION}/bin:$PATH

# Install NodeJS
ARG NODEJS_VERSION=v20.10.0
ARG NODE_PACKAGE=node-${NODEJS_VERSION}-linux-x64
ARG NODE_HOME=/opt/$NODE_PACKAGE
RUN curl https://nodejs.org/dist/${NODEJS_VERSION}/${NODE_PACKAGE}.tar.xz | tar xvfJ - -C /opt
ENV NODE_PATH ${NODE_HOME}/lib/node_modules
ENV PATH ${NODE_HOME}/bin:$PATH

# Install Bazel (via Bazelisk)
ARG BAZELISK_VERSION=v1.20.0
ARG BAZELISK_DOWNLOAD_SHA="d9af1fa808c0529753c3befda75123236a711d971d3485a390507122148773a3"
RUN curl -fSsL -o /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-amd64 \
    && echo "${BAZELISK_DOWNLOAD_SHA} /usr/local/bin/bazel" | sha256sum --check \
    && chmod 0755 /usr/local/bin/bazel

# Install Bazel Buildifer
ARG BUILDIFER_VERSION=v7.1.2
ARG BUILDIFER_DOWNLOAD_SHA="28285fe7e39ed23dc1a3a525dfcdccbc96c0034ff1d4277905d2672a71b38f13"
RUN curl -fSsL -o /usr/local/bin/buildifier https://github.com/bazelbuild/buildtools/releases/download/${BUILDIFER_VERSION}/buildifier-linux-amd64 \
    && echo "${BUILDIFER_DOWNLOAD_SHA} /usr/local/bin/buildifier" | sha256sum --check \
    && chmod 0755 /usr/local/bin/buildifier

# Install watchexec https://watchexec.github.io/downloads/watchexec/
ARG WATCHEXEC_VERSION=1.23.0
ARG WATCHEXEC_ARCH=x86_64
ADD https://github.com/watchexec/watchexec/releases/download/v${WATCHEXEC_VERSION}/watchexec-${WATCHEXEC_VERSION}-${WATCHEXEC_ARCH}-unknown-linux-gnu.deb /tmp/
ADD https://github.com/watchexec/watchexec/releases/download/v${WATCHEXEC_VERSION}/watchexec-${WATCHEXEC_VERSION}-${WATCHEXEC_ARCH}-unknown-linux-gnu.deb.sha256 /tmp/
RUN echo "`cat /tmp/watchexec-${WATCHEXEC_VERSION}-${WATCHEXEC_ARCH}-unknown-linux-gnu.deb.sha256` /tmp/watchexec-${WATCHEXEC_VERSION}-${WATCHEXEC_ARCH}-unknown-linux-gnu.deb" | sha256sum --check \
    && dpkg -i /tmp/watchexec-${WATCHEXEC_VERSION}-${WATCHEXEC_ARCH}-unknown-linux-gnu.deb
