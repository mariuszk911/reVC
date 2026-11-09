# Derived Bebbo toolchain image for the reVC AmigaOS 68k cross-build.
#
# The port plan assumed the `amigadev/m68k-amigaos-gcc:with-make` tag, but that
# tag is no longer published. The `:latest` image ships the full GCC 6.5 Bebbo
# toolchain plus CMake, but without `make`/`ninja`; this adds them so
# build/amigaos/build.sh can drive either the Makefile or CMake build.
#
# Build:
#   docker build -t revc/amigaos-toolchain:latest build/amigaos
FROM amigadev/m68k-amigaos-gcc:latest

RUN apt-get update \
 && apt-get install -y --no-install-recommends make ninja-build \
 && rm -rf /var/lib/apt/lists/*
