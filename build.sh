#!/bin/sh
# Builds software-ess-adapter on Linux and packs what end users need into
# dist/software-ess-adapter.tar.gz. Needs CMake, Ninja, g++ and the development packages listed in
# DEVELOPMENT.md.
#
# --output <name> names the archive instead: ./build.sh --output demo writes dist/demo.tar.gz.
# --dev builds the Ship of Harkinian features (SoH output and input lag), which stay out of release
# builds until SoH's DSU input is in an official libultraship release.
set -e
cd "$(dirname "$0")"

archive_name=software-ess-adapter
dev=OFF
while [ $# -gt 0 ]; do
    case "$1" in
    --output)
        [ $# -ge 2 ] || { echo "--output needs a name" >&2; exit 1; }
        archive_name="${2%.tar.gz}"
        shift 2
        ;;
    --dev)
        dev=ON
        shift
        ;;
    *)
        echo "Unknown argument '$1'. Usage: ./build.sh [--output <archive name>] [--dev]" >&2
        exit 1
        ;;
    esac
done

# Always pass SOH_DEV, so a build folder once configured with --dev doesn't keep it.
build=build/linux
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSOH_DEV=$dev"
cmake --build "$build" --target software-ess-adapter

stage="$build/package/software-ess-adapter"
rm -rf "$build/package"
mkdir -p "$stage/dolphin"
cp "$build/software-ess-adapter" "$stage"  # license texts and fonts are built in (About window)
cp "dolphin/Software ESS Adapter.ini" "$stage/dolphin"
cp linux/51-gcadapter.rules "$stage"

mkdir -p dist
tar -czf "dist/$archive_name.tar.gz" -C "$build/package" software-ess-adapter
kind="$build"
[ "$dev" = ON ] && kind="$build, dev"
echo "Built $(pwd)/dist/$archive_name.tar.gz ($kind)"
