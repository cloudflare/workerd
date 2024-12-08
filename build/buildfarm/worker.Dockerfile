FROM bazelbuild/buildfarm-worker:2.10.2

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install --no-upgrade --no-install-recommends -y wget software-properties-common
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - \
    && add-apt-repository -y "deb http://apt.llvm.org/lunar/ llvm-toolchain-lunar-17 main"

RUN apt-get update
RUN apt-get install --no-upgrade --no-install-recommends -y \
    clang-17 libc++-17-dev and libc++abi-17-dev


RUN rm /app/build_buildfarm/config.minimal.yml
COPY config.yml /app/build_buildfarm/config.minimal.yml
