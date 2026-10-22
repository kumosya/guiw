#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sdk=${OSINFO_SDK:-"${INFOIDE_HOME:-/usr/local/lib/osinfo/sdk}"}
test -f "$sdk/sdk.json"
mkdir -p "$sdk/include/osgui-c++/osgui"
cp lib/osgui-c++/include/osgui/*.h "$sdk/include/osgui-c++/osgui/"
cp lib/osgui-c++/build/packages/libosgui-c++/libosgui-c++.so "$sdk/lib/"
