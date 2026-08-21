#!/bin/sh
# Cross-build Basilisk II for NetBSD/mac68k, with the wscons DGA backend.
#
# Needs the m68k sysroot at /work/sysroot-mac68k and the toolchain at
# /work/tools, both from the host-OS tree's setup-sysroot.sh.
#
# Run from the repository root:
#   docker run --rm -v nbwork:/work -v $PWD:/out debian:12 sh /out/netbsd-port/build-basilisk.sh
set -eu

SYSROOT=/work/sysroot-mac68k
TOOLS=/work/tools
HOST=m68k--netbsdelf
SRC=/out/BasiliskII/src/Unix
BUILD=/work/basilisk-build

export DEBIAN_FRONTEND=noninteractive
echo "=== [1/4] build prerequisites (host side) ==="
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq --no-install-recommends \
	build-essential automake autoconf libtool pkg-config file make >/dev/null 2>&1

echo "=== [2/4] autogen ==="
cd "$SRC"
if [ ! -f configure ] || [ configure.ac -nt configure ]; then
	# -I m4 is required.  The tree bundles the macros it needs (esd.m4,
	# gtk.m4, gettext.m4 ...) but declares neither AC_CONFIG_MACRO_DIR
	# nor ACLOCAL_AMFLAGS, so aclocal never looks in m4/ and autoreconf
	# dies on "possibly undefined macro: AM_PATH_ESD" -- taking a couple
	# of unrelated AC_DEFINE/AC_MSG_WARN complaints down with it, which
	# are cascade noise rather than real problems.
	# ACLOCAL_PATH as well as -I: autoreconf's -I did not reach aclocal's
	# search path here, and aclocal.m4 came out with no AM_PATH_ESD in it
	# at all, so the macro survived into the generated configure as a
	# literal and died as a shell syntax error.
	ACLOCAL_PATH="$SRC/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	export ACLOCAL_PATH
	if ! autoreconf -fi -I m4 >/tmp/autogen.log 2>&1; then
		echo "  AUTORECONF FAILED - last 25 lines:"
		tail -25 /tmp/autogen.log | sed 's/^/    /'
		exit 1
	fi
fi
[ -f configure ] || {
	echo "  no configure produced"
	exit 1
}

echo "=== [3/4] cross configure ==="
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"

export PATH="$TOOLS/bin:$PATH"
export CC="${HOST}-gcc --sysroot=$SYSROOT"
export CXX="${HOST}-g++ --sysroot=$SYSROOT"
export LD="${HOST}-ld"
export AR="${HOST}-ar"
export RANLIB="${HOST}-ranlib"
export STRIP="${HOST}-strip"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/X11R7/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export CPPFLAGS="-I$SYSROOT/usr/include -I$SYSROOT/usr/X11R7/include"
export LDFLAGS="-L$SYSROOT/usr/lib -L$SYSROOT/usr/X11R7/lib -Wl,-rpath,/usr/X11R7/lib"

# Autoconf cannot RUN its probes when cross compiling, so anything using
# AC_TRY_RUN silently takes the "cross" branch or stops.  Seed the answers
# we know for this target rather than let it guess.  Each of these is a
# property of NetBSD/m68k, not of the build host.
: >config.cache

# Upstream already provides the escape hatch: an AC_ARG_VAR named
# BII_CROSS_* for every probe that would otherwise need to run.  Use those
# rather than hand-written cache entries.
#
# The load-bearing one is the sigcontext subterfuge.  sigsegv.cpp DOES
# carry a NetBSD/m68k case -- it includes <m68k/frame.h> and decodes the
# fault address out of the sigcontext -- but configure cannot run the
# probe that would find it, so it concludes the target has no fault
# recovery at all and defines neither SIGSEGV_FAULT_HANDLER_ARGLIST
# family.  The file then fails to compile on a macro sitting in its own
# source, which is a confusing way to be told the answer.
#
# The rest are plain properties of NetBSD.  Left at their conservative
# defaults on purpose: MAP_LOW_AREA, because NetBSD refuses to map page
# zero unless vm.user_va0_disable is cleared, and the two
# signal-reinstall guesses, which cost only a redundant reinstall if the
# guess is wrong.
export BII_CROSS_HAVE_SIGCONTEXT_SUBTERFUGE=yes
export BII_CROSS_SOCKLEN_T=yes
export BII_CROSS_MMAP_ANON=yes
export BII_CROSS_MPROTECT_WORKS=yes

# --disable-sdl-video is NOT redundant: configure.ac's help text claims it
# defaults to no while the code defaults it to yes, and enabling it turns
# off every direct-framebuffer path including ours.
"$SRC/configure" \
	--host="$HOST" \
	--build=x86_64-pc-linux-gnu \
	--cache-file=config.cache \
	--disable-sdl-video \
	--disable-sdl-audio \
	--enable-wscons-dga \
	--disable-vosf \
	--without-mon \
	--without-gtk \
	--without-esd \
	>configure.log 2>&1 || {
	echo "  CONFIGURE FAILED - last 30 lines:"
	tail -30 configure.log | sed 's/^/    /'
	exit 1
}

echo "  configured.  What it settled on:"
grep -iE 'Running m68k code natively|wscons DGA support|fbdev DGA support|XFree86 DGA|SDL|Enable video on SEGV' \
	configure.log | tail -8 | sed 's/^/    /'

echo "=== [4/4] build ==="
make -j"$(nproc)" >build.log 2>&1 || {
	echo "  BUILD FAILED - last 40 lines:"
	tail -40 build.log | sed 's/^/    /'
	exit 1
}

BIN=$(find . -name BasiliskII -type f -perm -u+x | head -1)
[ -n "$BIN" ] || {
	echo "  no BasiliskII binary produced"
	exit 1
}
cp "$BIN" /out/netbsd-port/BasiliskII
"$TOOLS/bin/${HOST}-objdump" -f "$BIN" | sed -n '2p' | sed 's/^/    /'
ls -l /out/netbsd-port/BasiliskII | sed 's/^/    /'
echo "=== done ==="
