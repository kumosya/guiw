INFOIDE ?= infoide
.PHONY: all package
all package:
	INFOIDE="$(INFOIDE)" $(if $(OSINFO_SDK),OSINFO_SDK="$(OSINFO_SDK)") sh "tools/build-release.sh"
	make -C lib/osgui-c++ package
	make -C app/tools package
	make -C app/editor package

install:
	INFOIDE="$(INFOIDE)" $(if $(OSINFO_SDK),OSINFO_SDK="$(OSINFO_SDK)") sh "tools/install-sdk.sh"
