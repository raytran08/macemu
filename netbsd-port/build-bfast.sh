#!/bin/sh
# Build the bfast kernel module against the cached NetBSD source and
# toolchain in the /work volume.  Run inside the Debian container:
#   docker run --rm -v nbwork:/work -v "$PWD":/out debian:12 \
#       sh /out/netbsd-port/build-bfast.sh
set -eu

echo "=== overlaying bfast into the module tree ==="
mkdir -p /work/usr/src/sys/modules/bfast
cp /out/netbsd-port/kmod/bfast/bfast.c \
	/out/netbsd-port/kmod/bfast/Makefile \
	/work/usr/src/sys/modules/bfast/

echo "=== building bfast.kmod ==="
cd /work/usr/src/sys/modules/bfast
/work/tools/bin/nbmake-mac68k dependall
cp bfast.kmod /out/netbsd-port/
ls -l /out/netbsd-port/bfast.kmod
echo "=== done ==="
