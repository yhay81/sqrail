FROM mcr.microsoft.com/devcontainers/cpp:2-ubuntu-24.04@sha256:2c413470d822e147ef6f2f53f1c2001940fedbb35f39e040ec379ec9a160070f

RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install --yes --no-install-recommends \
        clang-18 \
        clang-format-18 \
        clang-tidy-18 \
        lldb-18 \
        llvm-18 \
        ninja-build \
        shellcheck \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 180 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 180 \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 180 \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-18 180 \
    && update-alternatives --install /usr/bin/llvm-profdata llvm-profdata /usr/bin/llvm-profdata-18 180 \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang
ENV CXX=clang++
