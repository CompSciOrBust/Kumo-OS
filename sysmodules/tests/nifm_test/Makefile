all: nx_release
nx_release:
	$(MAKE) -f Makefile.native

.PHONY: all nx_release clean install
clean:
	$(MAKE) -f Makefile.native clean

install: nx_release
	cp out/nintendo_nx_arm64_armv8a/release/nifm_test.kip ../../../kips/
