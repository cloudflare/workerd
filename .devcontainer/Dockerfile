FROM mcr.microsoft.com/vscode/devcontainers/base:dev-bookworm

# Install dependencies, including clang via through LLVM APT repository. Note that this
# will also install lldb and clangd alongside dependencies.
ARG LLVM_VERSION=18
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install software-properties-common python3 python3-distutils tclsh nodejs npm \
    && curl -fSsL -o /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && bash /tmp/llvm.sh ${LLVM_VERSION} \
    && apt-get -y install --no-install-recommends libunwind-${LLVM_VERSION} libc++abi1-${LLVM_VERSION} libc++1-${LLVM_VERSION} libc++-${LLVM_VERSION}-dev libclang-rt-${LLVM_VERSION}-dev -o DPkg::options::="--force-overwrite"
ENV PATH /usr/lib/llvm-${LLVM_VERSION}/bin:$PATH

# Install Bazel (via Bazelisk)
RUN npm install -g @bazel/bazelisk
