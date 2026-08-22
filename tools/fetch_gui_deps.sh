#!/bin/sh
# Fetches the GUI's third-party sources at the revisions this project was
# built against. They are not committed: they are large, they are not ours,
# and pinning the revision here keeps the build reproducible without carrying
# a copy of Dear ImGui in the history.
#
#     sh tools/fetch_gui_deps.sh
#
# The solver, the CLI, the benchmark and every test build without any of this.
set -e

CIMGUI_URL=https://github.com/cimgui/cimgui.git
CIMGUI_REV=700140771a12e61f4bb851fb4e2d885f04b32dd9

root=$(dirname "$0")/..
mkdir -p "$root/third_party"

if [ -d "$root/third_party/cimgui/.git" ]; then
    echo "third_party/cimgui already present"
else
    git clone --recursive "$CIMGUI_URL" "$root/third_party/cimgui"
fi

git -C "$root/third_party/cimgui" fetch --recurse-submodules origin
git -C "$root/third_party/cimgui" checkout --recurse-submodules "$CIMGUI_REV"
echo "cimgui at $CIMGUI_REV, with Dear ImGui as its submodule"
