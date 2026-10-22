#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
infoide=${INFOIDE:-infoide}
sdk=${OSINFO_SDK:-"${INFOIDE_HOME:-/usr/local/lib/osinfo}/sdk"}
for project in lib/osgui-c++ . app/tools app/editor; do
    "$infoide" pkg --sdk "$sdk" --project "$project"
done
mkdir -p build/release
cp lib/osgui-c++/build/packages/libosgui-c++.inpk build/release/
cp build/packages/guiw.inpk build/release/
"$infoide" bundle --output build/release/guiw-app.zip \
    app/tools/build/packages/guiw-app-tools.inpk app/editor/build/packages/guiw-editor.inpk
