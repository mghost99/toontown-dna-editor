#!/bin/bash
# Linux install: builds the editor, installs icons + launcher + .desktop entry, and records the resources folder.
# Usage: installer/install.sh [RESOURCES_DIR]   (default: auto-detect, else a folder picker / prompt)
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C "$HERE/build" >/dev/null
for s in 16 32 48 64 128 256 512; do
  install -Dm644 "$HERE/assets/branding/dna-editor_$s.png" "$HOME/.local/share/icons/hicolor/${s}x${s}/apps/dna-editor.png"
done
rm -f "$HOME/.local/share/icons/hicolor/scalable/apps/dna-editor.svg"
gtk-update-icon-cache -q "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
REPO="$HERE"
mkdir -p "$HOME/.local/bin"
cat > "$HOME/.local/bin/dna-editor" <<EOS
#!/bin/bash
# Starts the native DNA Editor (installed by installer/install.sh)
export DISPLAY="\${DISPLAY:-:1}"
exec "$HERE/run.sh" "\$@"
EOS
chmod +x "$HOME/.local/bin/dna-editor"
install -Dm644 /dev/stdin "$HOME/.local/share/applications/dna-editor.desktop" <<EOS
[Desktop Entry]
Type=Application
Name=DNA Editor
GenericName=Level Editor
Comment=Open-source 3D/2D Toontown DNA level editor
Exec=$HOME/.local/bin/dna-editor
Path=$REPO
Icon=dna-editor
Terminal=false
StartupWMClass=dna-editor
Categories=Development;Game;
Keywords=toontown;dna;editor;
EOS
update-desktop-database -q "$HOME/.local/share/applications" 2>/dev/null || true
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/dna-editor/config.ini"
RES="$1"
if [ -z "$RES" ] && [ -f "$CFG" ]; then RES="$(sed -n 's/^resources=//p' "$CFG" | head -1)"; fi
if [ -z "$RES" ]; then for c in "$REPO/resources" "$HOME/resources"; do
  [ -d "$c/phase_4" ] || [ -d "$c/phase_8" ] && { RES="$c"; break; }; done; fi
if [ -z "$RES" ] && command -v zenity >/dev/null; then RES="$(zenity --file-selection --directory --title='Game resources folder' 2>/dev/null || true)"; fi
if [ -z "$RES" ]; then read -r -p "Resources folder: " RES; fi
mkdir -p "$(dirname "$CFG")"; printf '[editor]\nresources=%s\n' "$RES" > "$CFG"
echo "installed: ~/.local/bin/dna-editor, desktop entry, icons; resources=$RES ($CFG)"
