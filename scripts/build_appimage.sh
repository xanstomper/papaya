#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================="
echo " Building Papaya Standalone AppImage Bundle"
echo "=========================================================="

APP_DIR="./AppDir"
BUILD_DIR="./build"

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/usr/bin"
mkdir -p "$APP_DIR/usr/lib"
mkdir -p "$APP_DIR/usr/share/papaya"
mkdir -p "$APP_DIR/usr/share/applications"
mkdir -p "$APP_DIR/usr/share/icons/hicolor/256x256/apps"

# 1. Compile C++23 Release Binaries
cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPAPAYA_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR"

# 2. Copy binaries and assets
cp "$BUILD_DIR/src/app/papaya" "$APP_DIR/usr/bin/papaya"
cp src/orchestrator/python/papaya_daemon.py "$APP_DIR/usr/bin/papaya-daemon"
chmod +x "$APP_DIR/usr/bin/papaya-daemon"
cp src/orchestrator/data/compatibility_db.json "$APP_DIR/usr/share/papaya/compatibility_db.json"
cp packaging/org.papaya.Papaya.desktop "$APP_DIR/org.papaya.Papaya.desktop"
cp packaging/org.papaya.Papaya.desktop "$APP_DIR/usr/share/applications/org.papaya.Papaya.desktop"

# 3. Create AppRun launcher script
cat << 'EOF' > "$APP_DIR/AppRun"
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "${0}")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH:-}"
export PAPAYA_HOME="${HOME}/Papaya"
exec "${HERE}/usr/bin/papaya" "$@"
EOF
chmod +x "$APP_DIR/AppRun"

echo "AppDir populated successfully at: $APP_DIR"
echo "To package into .AppImage, run: appimagetool $APP_DIR Papaya-x86_64.AppImage"
