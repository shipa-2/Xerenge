#!/bin/bash
# Packs the UWP build (build-uwp, configured with -DREX_UWP=ON) into a signed
# .msix for the Xbox One in Dev Mode. Runs on Windows (Git bash), with the
# Windows SDK's makeappx and signtool.
#
#   uwp/build_package.sh [--game <extracted game folder>] [--out <file.msix>]
#
# --game puts the game into the package (its `game` folder, where the app
# looks first); without it the app reads LocalState\game, copied there through
# Device Portal. The game's files never leave the machine but in the package.
# Install it through Device Portal (Home > Add), then set the app's type to
# Game in Dev Home: as an App it gets 1 GB and no D3D12.
set -e
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build-uwp}
OUT=$BUILD/Xerenge.msix
GAME=
while [ $# -gt 0 ]; do
    case $1 in
        --game) GAME=$2; shift ;;
        --out) OUT=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

KITS="/c/Program Files (x86)/Windows Kits/10/bin"
SDKBIN=$(ls -d "$KITS"/10.*/x64 | sort -V | tail -1)
MAKEAPPX="$SDKBIN/makeappx.exe"
SIGNTOOL="$SDKBIN/signtool.exe"
# The C++ runtime the build linked, in its OneCore form, beside the game.
CRT=${CRT:-$(ls -d "/c/Program Files"*"/Microsoft Visual Studio/"*/*/VC/Redist/MSVC/14.*/onecore/x64/Microsoft.VC14*.CRT 2>/dev/null | sort -V | tail -1)}
[ -x "$MAKEAPPX" ] || { echo "makeappx not found under $KITS" >&2; exit 1; }
[ -d "$CRT" ] || { echo "OneCore C++ runtime not found (set CRT=)" >&2; exit 1; }
for f in burnout.exe rexruntime.dll rexgpu-plume.dll; do
    [ -f "$BUILD/$f" ] || { echo "$BUILD/$f missing: build $BUILD first" >&2; exit 1; }
done
if [ -n "$GAME" ] && [ ! -f "$GAME/default.xex" ]; then
    echo "$GAME has no default.xex" >&2
    exit 1
fi

STAGE=$BUILD/package
rm -rf "$STAGE"
mkdir -p "$STAGE"
version=$(tr -d '[:space:]' < ../VERSION).0
sed "s/@VERSION@/$version/" uwp/AppxManifest.xml.in > "$STAGE/AppxManifest.xml"

win() { cygpath -w "$(realpath "$1")"; }
map=$STAGE/files.map
{
    echo "[Files]"
    echo "\"$(win "$STAGE/AppxManifest.xml")\" \"AppxManifest.xml\""
    for f in burnout.exe rexruntime.dll rexgpu-plume.dll; do
        echo "\"$(win "$BUILD/$f")\" \"$f\""
    done
    for f in vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll; do
        echo "\"$(win "$CRT/$f")\" \"$f\""
    done
    for f in uwp/Assets/*.png; do
        echo "\"$(win "$f")\" \"Assets\\$(basename "$f")\""
    done
    if [ -n "$GAME" ]; then
        (cd "$GAME" && find . -type f -not -path "./\$systemupdate/*") | sed 's|^\./||' | while read -r rel; do
            echo "\"$(cygpath -w "$(realpath "$GAME")")\\${rel//\//\\}\" \"game\\${rel//\//\\}\""
        done
    fi
} > "$map"
echo "packing $(($(wc -l < "$map") - 1)) files, version $version"

# The game is mostly compressed already; packing it again takes long for little.
nc=
[ -n "$GAME" ] && nc=//nc
"$MAKEAPPX" pack //o $nc //f "$(win "$map")" //p "$(cygpath -w "$(realpath -m "$OUT")")" > "$STAGE/makeappx.log" ||
    { tail -20 "$STAGE/makeappx.log"; exit 1; }

# A self-signed certificate for CN=Xerenge, made once and kept outside the tree.
CERT_DIR=${CERT_DIR:-$HOME/.xerenge-uwp}
mkdir -p "$CERT_DIR"
if [ ! -f "$CERT_DIR/xerenge.pfx" ]; then
    powershell -NoProfile -Command "\$c = New-SelfSignedCertificate -Type Custom -Subject 'CN=Xerenge' -KeyUsage DigitalSignature -CertStoreLocation Cert:\\CurrentUser\\My -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}'); Export-PfxCertificate -Cert \$c -FilePath '$(cygpath -w "$CERT_DIR")\\xerenge.pfx' -Password (ConvertTo-SecureString -String 'xerenge' -Force -AsPlainText) | Out-Null; Export-Certificate -Cert \$c -FilePath '$(cygpath -w "$CERT_DIR")\\xerenge.cer' | Out-Null"
fi
"$SIGNTOOL" sign //fd SHA256 //f "$(cygpath -w "$CERT_DIR/xerenge.pfx")" //p xerenge "$(cygpath -w "$(realpath "$OUT")")" > "$STAGE/signtool.log" ||
    { cat "$STAGE/signtool.log"; exit 1; }
cp "$CERT_DIR/xerenge.cer" "$(dirname "$OUT")/"
ls -la "$OUT"
echo "certificate: $(dirname "$OUT")/xerenge.cer"
