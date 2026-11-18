all: nx_release
nx_release:
	$(MAKE) -f Makefile.native

exefs: nx_release
	mkdir -p out/exefs
	cp out/nintendo_nx_arm64_armv8a/release/hello_program.nso out/exefs/main
	cp out/nintendo_nx_arm64_armv8a/release/hello_program.npdm out/exefs/main.npdm

clean:
	$(MAKE) -f Makefile.native clean
	rm -rf out/exefs

.PHONY: all nx_release exefs clean
