# Build the BSD system module with the project IPC runtime and libnx.
.DEFAULT_GOAL := nx_release

.PHONY: nx_release nx_debug nx_audit native install clean
nx_release:
	$(MAKE) -f Makefile.native VARIANT=release
nx_debug:
	$(MAKE) -f Makefile.native VARIANT=debug
nx_audit:
	$(MAKE) -f Makefile.native VARIANT=audit
native: nx_release
install:
	$(MAKE) -f Makefile.native VARIANT=release out/nintendo_nx_arm64_armv8a/release/bsdsockets.kip
	cp out/nintendo_nx_arm64_armv8a/release/bsdsockets.kip ../../kips/bsdsockets.kip
clean:
	$(MAKE) -f Makefile.native VARIANT=release clean
	$(MAKE) -f Makefile.native VARIANT=debug clean
	$(MAKE) -f Makefile.native VARIANT=audit clean
