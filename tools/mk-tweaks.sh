#!/bin/bash
# Build this port's tweaks as packages of the jailbreak prefix, so that Sileo lists them as
# installed and can remove them.
#
#   tools/mk-tweaks.sh            builds and installs into jb/root/var/jb (the prefix the system
#                                 image carries as /jb; init copies it to /var/jb at every boot)
#   KEEP_DEBS=1 tools/mk-tweaks.sh  also leaves the .deb files in local/tweaks/
#
# Needs the same toolchain as userland/build.sh (THEOS with the iPhoneOS16.5 SDK).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
THEOS=${THEOS:-$HOME/theos}
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
PREFIX=$ROOT/jb/root/var/jb
OUT=$ROOT/local/tweaks
# userland/build.sh finds its compiler through this directory; a bare CI shell does not.
export PATH="$ROOT/xnu-deps/host/bin:$PATH"
DYN="xnu-clang -arch arm64 -isysroot $SDK -O1 -fno-omit-frame-pointer"

[ -d "$PREFIX" ] || { echo "mk-tweaks.sh: no $PREFIX -- run tools/mk-jb.sh first" >&2; exit 1; }
[ -d "$SDK" ] || { echo "mk-tweaks.sh: no SDK at $SDK (THEOS=$THEOS)" >&2; exit 1; }
command -v xnu-clang >/dev/null || { echo "mk-tweaks.sh: xnu-clang not found (looked in $ROOT/xnu-deps/host/bin); run xnu-deps/setup.sh --stage1" >&2; exit 1; }
mkdir -p "$OUT"

# One package, one line here: name version dylib source-filter-bundles
#
#   RedwoodIsland -- the island: a black pill over the status bar in SpringBoard plus the
#   MobileGestalt answers that say the device has one. The work itself is in
#   /redwood/lib/libredwood-sb.dylib, which the port injects into SpringBoard; this package makes
#   the same thing a package, loaded through ElleKit like any other tweak.
#
#   RedwoodExc -- the diagnostic: every app of the 21:03 run died of an uncaught exception thrown
#   inside a MediaPlayer block, and an app's stderr never reaches the kernel log. This interposes
#   objc_exception_throw in EVERY process (empty filter) and writes the exception's name, reason and
#   frames to /dev/console -- which is in the log we fetch. It is meant to be removed once the
#   exception is known; it is a question, not a feature.
PKGS=(
	"redwood.island 1.0.0 tweak_island com.apple.springboard the island as a tweak"
	"redwood.exc    1.0.0 tweak_exc    -                    a line per exception thrown, for diagnosis"
)

status_line() {                                  # $1 = package id, $2 = version
	printf 'Package: %s\nVersion: %s\nArchitecture: iphoneos-arm64\n' "$1" "$2"
	printf 'Maintainer: Redwood Ace 5 <redwood@localhost>\n'
	printf 'Section: Tweaks\nPriority: optional\n'
	printf 'Depends: ellekit (>= 0.4.5)\n'
	printf 'Description: HackiOS Ace 5: %s\n' "$3"
}

for spec in "${PKGS[@]}"; do
	set -- $spec
	id=$1; ver=$2; src=$3; bundle=$4; what=${5:-the island as a tweak}
	name=${id#redwood.}                          # island
	lib=${name^}                                 # Island -> RedwoodIsland.dylib
	dylib="Redwood${lib}.dylib"
	work=$(mktemp -d)

	echo "== $id $ver -> $dylib"

	# the tweak itself
	$DYN -dynamiclib -install_name "/var/jb/Library/MobileSubstrate/DynamicLibraries/$dylib" \
		-o "$work/$dylib" "$ROOT/userland/src/$src.c"

	# the layout a rootless prefix uses: everything under /var/jb
	stage=$work/stage
	mkdir -p "$stage/DEBIAN" "$stage/var/jb/Library/MobileSubstrate/DynamicLibraries"
	cp "$work/$dylib" "$stage/var/jb/Library/MobileSubstrate/DynamicLibraries/"
	# a filter of "-" means every process: an empty Filter list matches any bundle, which is what
	# the exception reporter needs and what the island does not (it is SpringBoard's)
	if [ "$bundle" = "-" ]; then
		cat > "$stage/var/jb/Library/MobileSubstrate/DynamicLibraries/${dylib%.dylib}.plist" <<PLIST
{ Filter = { }; }
PLIST
	else
		cat > "$stage/var/jb/Library/MobileSubstrate/DynamicLibraries/${dylib%.dylib}.plist" <<PLIST
{ Filter = { Bundles = ( "$bundle" ); }; }
PLIST
	fi
	cp -f "$stage/var/jb/Library/MobileSubstrate/DynamicLibraries/${dylib%.dylib}.plist" "$work/"
	{
		status_line "$id" "$ver" "$what"
		echo "Installed-Size: 16"
	} > "$stage/DEBIAN/control"

	# a .deb is an ar archive: debian-binary, control.tar.gz, data.tar.gz
	( cd "$stage" && tar -czf "$work/control.tar.gz" ./DEBIAN )
	( cd "$stage" && tar -czf "$work/data.tar.gz" ./var )
	printf '2.0\n' > "$work/debian-binary"
	( cd "$work" && ar rcs "$OUT/${id}_${ver}_iphoneos-arm64.deb" debian-binary control.tar.gz data.tar.gz )
	echo "   $OUT/${id}_${ver}_iphoneos-arm64.deb"

	# into the prefix the image carries: the files, then dpkg's own record of it, which is what
	# Sileo reads to show a package as installed
	# The prefix is rootless: Library/MobileSubstrate/DynamicLibraries is a SYMLINK (to
	# usr/lib/TweakInject), so a plain "cp -a dir/. prefix/" fails with "cannot overwrite
	# non-directory ... with directory" -- which is exactly what the 13:00 build died of. Resolve the
	# link and put the two files where it points.
	dllib="$PREFIX/Library/MobileSubstrate/DynamicLibraries"
	# The link's target is an absolute iOS path (/var/jb/usr/lib/TweakInject), which means
	# nothing on this machine -- and /var/jb does not exist here, so readlink -f returns empty
	# and mkdir -p on the dangling link fails with "File exists" (the 13:10 build). Map the
	# target back into the prefix by hand instead of resolving it against the host.
	real=$dllib
	if [ -L "$dllib" ]; then
		t=$(readlink "$dllib")
		case "$t" in
		/var/jb/*) real="$PREFIX/${t#/var/jb/}" ;;
		/*)        real="$PREFIX$t" ;;
		*)         real=$(dirname "$dllib")/"$t" ;;
		esac
		echo "   ($dllib -> $t, so into $real)"
	fi
	mkdir -p "$real"
	cp -f "$work/$dylib" "$real/"
	cp -f "$work/${dylib%.dylib}.plist" "$real/"
	chmod 755 "$real/$dylib"
	echo "   -> $(readlink -f "$real")/$dylib"
	{
		status_line "$id" "$ver" "$what"
		echo "Status: install ok installed"
		echo "Installed-Size: 16"
		echo
	} >> "$PREFIX/Library/dpkg/status"
	rm -rf "$work"
	[ -n "${KEEP_DEBS:-}" ] || rm -f "$OUT/${id}_${ver}_iphoneos-arm64.deb"
done

echo "mk-tweaks.sh: installed into $PREFIX"
ls -1 "$PREFIX/Library/MobileSubstrate/DynamicLibraries" 2>/dev/null || true
