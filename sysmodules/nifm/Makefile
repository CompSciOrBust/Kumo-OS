# Build the NIFM system module with the project IPC runtime and libnx.
.DEFAULT_GOAL := nx_release

.PHONY: nx_release nx_debug nx_audit native install clean
nx_release:
	$(MAKE) -f Makefile.native VARIANT=release
nx_debug:
	$(MAKE) -f Makefile.native VARIANT=debug
nx_audit:
	$(MAKE) -f Makefile.native VARIANT=audit
native: nx_release
install: nx_release
	cp out/nintendo_nx_arm64_armv8a/release/nifm.kip ../../kips/nifm.kip
clean:
	$(MAKE) -f Makefile.native VARIANT=release clean
	$(MAKE) -f Makefile.native VARIANT=debug clean
	$(MAKE) -f Makefile.native VARIANT=audit clean
