#!/usr/bin/env bash
# Builds anomaly_fsr.dll with MinGW-w64 and packs a Mod Organizer 2 installable archive:
#   dist/Anomaly-FSR1-<version>.zip
#     bin/anomaly_fsr.dll
#     gamedata/scripts/anomaly_fsr.script, anomaly_fsr_mcm.script
#     gamedata/configs/text/eng/ui_st_anomaly_fsr.xml
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(anomaly_fsr VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build

STAGE=build/package
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" dist
cp -R mod/gamedata "$STAGE/"
cp build/anomaly_fsr.dll "$STAGE/bin/"
cp README.md "$STAGE/Anomaly FSR1 README.md"
cp third_party/fsr1/LICENSE.txt "$STAGE/Anomaly FSR1 LICENSE (AMD FidelityFX).txt"

ARCHIVE="dist/Anomaly-FSR1-$VERSION.zip"
rm -f "$ARCHIVE"
(cd "$STAGE" && zip -r -X -q "../../$ARCHIVE" . -x "*.DS_Store")
echo "Wrote $ARCHIVE"
