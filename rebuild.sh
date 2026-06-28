#!/bin/zsh
set -e

cd "$(dirname "$0")"

echo "Building..."
ninja -C build-ai -j$(sysctl -n hw.logicalcpu) gittyup

echo "Installing to /Applications..."
osascript -e 'quit app "Gittyup-dev"' 2>/dev/null; sleep 1
rm -rf /Applications/Gittyup-dev.app
cp -R build-ai/gittyup.app /Applications/Gittyup-dev.app
mdimport /Applications/Gittyup-dev.app

echo "Done — launching"
open /Applications/Gittyup-dev.app
