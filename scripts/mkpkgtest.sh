#!/bin/sh
# Phase 7B test packages: build the signed bundles the installer gates use.
#
#   mkpkgtest.sh OUT_DIR CAPTEST_ELF MKPKG
#
# OUT_DIR receives:
#   captest.crpkg    signed by PKG_SEED, whose public key lands in keys.conf
#   untrusted.crpkg  signed by PKG_BAD_SEED, so the trust list has no match
#   keys.conf        the trust list (/System/trusted/keys.conf)
#   perms.conf       the capability policy (/System/perms.conf)
#   tree/            the unsigned bundle both packages were built from
#
# The package ids, versions and timestamps are fixed so two runs of the same
# source tree produce byte-identical images.
set -eu

out=${1:?usage: mkpkgtest.sh OUT_DIR CAPTEST_ELF MKPKG}
captest=${2:?usage: mkpkgtest.sh OUT_DIR CAPTEST_ELF MKPKG}
mkpkg=${3:?usage: mkpkgtest.sh OUT_DIR CAPTEST_ELF MKPKG}
: "${PKG_SEED:?PKG_SEED must be set}"
: "${PKG_BAD_SEED:?PKG_BAD_SEED must be set}"

tree=$out/tree/captest.app
rm -rf "$out/tree"
mkdir -p "$tree/bin/x86_64" "$out"
cp "$captest" "$tree/bin/x86_64/captest"

printf '%s\n' \
    '[app]' \
    'id = "captest"' \
    'name = "CapTest"' \
    'version = "1.0.0"' \
    'publisher = "Crug Labs"' \
    '' \
    '[exec]' \
    'prog = "bin/x86_64/captest"' \
    '' \
    '[caps]' \
    'require = "fs.read"' \
    '' \
    '[tooltypes]' \
    'MODE = "probe"' \
    '' \
    '[icon]' \
    'rgb = "FFB000"' > "$tree/manifest.toml"

printf '%s\n' '# CrugOS trusted package keys (phase 7B).' > "$out/keys.conf"
"$mkpkg" --seed "$PKG_SEED" --out "$out/captest.crpkg" --bundle "$tree" \
    --id captest --name 'CapTest' --version 1.0.0 --publisher 'Crug Labs' \
    --caps fs.read --key-id crug-test --timestamp 0 \
    --trust-out "$out/keys.conf"

rm -f "$out/untrusted.crpkg"
"$mkpkg" --seed "$PKG_BAD_SEED" --out "$out/untrusted.crpkg" --bundle "$tree" \
    --id captest --name 'CapTest' --version 1.0.0 --publisher 'Crug Labs' \
    --caps pkg.install --key-id crug-rogue --timestamp 0

printf '%s\n' \
    '# CrugOS capability policy (phase 7B).' \
    'default app.launch,fs.read' \
    'captest fs.read' > "$out/perms.conf"

echo "mkpkgtest: $out/captest.crpkg $out/untrusted.crpkg $out/keys.conf $out/perms.conf"
