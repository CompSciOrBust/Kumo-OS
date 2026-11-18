#!/usr/bin/env python3
"""Build the whole OS and package it as one bootable disk image.

    scripts/build-os.py                      # build what os.conf lists -> build/os/default/disk.img
    scripts/build-os.py configs/minimal.conf # another configuration -> build/os/minimal/disk.img
    scripts/build-os.py --list               # show what a configuration would build, and stop
    scripts/build-os.py --no-kernel          # reuse build/kernel.bin and build/BOOTAA64.EFI

It applies the patches, builds the bootloader and the kernel, builds every
system module the configuration names, and writes a single disk image:

  * an EFI system partition with the bootloader, the kernel and the modules
    that are KIPs (started by the kernel at boot);
  * Kumo's system partitions, with every module that is an ordinary program
    packaged as NCAs and installed in SYSTEM's content storage, where ncm
    indexes them and pm and the loader start them. Programs marked "boot"
    are listed in SYSTEM:/boot.lst, which the launcher module reads at boot.

Whether a module is a KIP or a program is read from its own build files, so
the configuration only lists names. The image boots in QEMU
(scripts/run-os.sh).

Configuration file: one entry per line, '#' starts a comment.

    module atmosphere/sm        # a module from vendor/atmosphere/stratosphere
    module fs                   # a module from sysmodules/
    module hello_program version=2 boot   # "boot": started at boot by the launcher
    size   1G                   # image size (default 1G)
    layout SYSTEM:256M:fat,...  # partitions after the EFI one (default below)

Needs devkitA64 with the switch tools (set DEVKITPRO),
clang and lld for the bootloader, and python3.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATMOSPHERE = os.path.join(ROOT, "vendor", "atmosphere")
LIBSTRATOSPHERE = os.path.join(ATMOSPHERE, "libraries", "libstratosphere", "lib",
                               "nintendo_nx_arm64_armv8a", "release", "libstratosphere.a")
RELEASE_DIR = os.path.join("out", "nintendo_nx_arm64_armv8a", "release")

DEFAULT_SIZE = "1G"
DEFAULT_LAYOUT = "PRODINFO:4M:raw,PRODINFOF:4M:fat,SAFE:64M:fat,SYSTEM:256M:fat,USER:256M:fat,SDCARD:rest:fat"


def fail(message):
    print(f"build-os: {message}", file=sys.stderr)
    sys.exit(1)


def run(command, **kwargs):
    """Run a command, echoing it; stop the build if it fails."""
    print("+ " + " ".join(command), flush=True)
    result = subprocess.run(command, cwd=ROOT, **kwargs)
    if result.returncode != 0:
        fail(f"command failed ({result.returncode}): {' '.join(command)}")


class Module:
    """One system module named in the configuration."""

    def __init__(self, source, options):
        self.source = source
        self.options = options
        if source.startswith("atmosphere/"):
            self.name = source.split("/", 1)[1]
            self.directory = os.path.join(ATMOSPHERE, "stratosphere", self.name)
        else:
            # Ours: sysmodules/<name>, or a test module in sysmodules/tests/<name>. "tests/<name>"
            # names the test one explicitly.
            self.name = os.path.basename(source)
            self.directory = os.path.join(ROOT, "sysmodules", source)
            test_directory = os.path.join(ROOT, "sysmodules", "tests", source)
            if not os.path.isfile(os.path.join(self.directory, "Makefile")) and os.path.isfile(os.path.join(test_directory, "Makefile")):
                self.directory = test_directory
        if not os.path.isfile(os.path.join(self.directory, "Makefile")):
            fail(f"module '{source}': no Makefile in {os.path.relpath(self.directory, ROOT)}")

        # Atmosphere templates declare their targets; native programs declare
        # their kind in Makefile.native. Other modules default to KIPs.
        self.uses_atmosphere_templates = os.path.isfile(os.path.join(self.directory, "system_module.mk"))
        self.kind = "kip"
        if self.uses_atmosphere_templates:
            with open(os.path.join(self.directory, "system_module.mk")) as f:
                match = re.search(r"^ATMOSPHERE_SYSTEM_MODULE_TARGETS\s*:=\s*(\w+)", f.read(), re.MULTILINE)
            if match and match.group(1) == "nsp":
                self.kind = "program"
        else:
            native_makefile = os.path.join(self.directory, "Makefile.native")
            if os.path.isfile(native_makefile):
                with open(native_makefile) as f:
                    if re.search(r"^HORIZON_MODULE_KIND\s*:=\s*program\s*$", f.read(), re.MULTILINE):
                        self.kind = "program"

    def output(self, extension):
        # Native libnx builds preserve the standard release layout.
        if self.uses_atmosphere_templates or os.path.isfile(os.path.join(self.directory, "Makefile.native")):
            return os.path.join(self.directory, RELEASE_DIR, f"{self.name}.{extension}")
        return os.path.join(self.directory, "out", f"{self.name}.{extension}")

    def program_id(self):
        """A program's ID, from its process descriptor (or id= in the configuration)."""
        if "id" in self.options:
            return int(self.options["id"], 0)
        descriptor = os.path.join(self.directory, f"{self.name}.json")
        try:
            with open(descriptor) as f:
                data = json.load(f)
            return int(data.get("program_id", data.get("title_id")), 0)
        except (OSError, TypeError, ValueError) as error:
            fail(f"module '{self.source}': cannot read a program ID from {descriptor} ({error}); add id=0x...")

    def build(self, checked_libstratosphere):
        if self.uses_atmosphere_templates:
            command = ["make", "-C", self.directory, "nx_release"]
            if checked_libstratosphere:
                # Without this, make re-checks and can rebuild all of libstratosphere for every module.
                command.append("ATMOSPHERE_CHECKED_LIBSTRATOSPHERE=1")
        else:
            command = ["make", "-C", self.directory]
        run(command)


def read_configuration(path):
    modules, settings = [], {"size": DEFAULT_SIZE, "layout": DEFAULT_LAYOUT}
    seen = set()
    with open(path) as f:
        for number, line in enumerate(f, 1):
            words = line.split("#", 1)[0].split()
            if not words:
                continue
            keyword, arguments = words[0], words[1:]
            if keyword == "module" and arguments:
                # Options are key=value, or a bare word, which is a flag.
                options = dict(word.split("=", 1) if "=" in word else (word, "") for word in arguments[1:])
                unknown = set(options) - {"id", "version", "boot"}
                if unknown:
                    fail(f"{path}:{number}: unknown option '{sorted(unknown)[0]}'")
                if arguments[0] in seen:
                    fail(f"{path}:{number}: module '{arguments[0]}' is listed twice")
                seen.add(arguments[0])
                modules.append(Module(arguments[0], options))
            elif keyword in settings and len(arguments) == 1:
                settings[keyword] = arguments[0]
            else:
                fail(f"{path}:{number}: cannot understand '{line.strip()}'")
    if not modules:
        fail(f"{path} lists no modules")
    return modules, settings


def check_dependencies(modules):
    """Warn about combinations that are known not to work; they are easy to get wrong."""
    names = {module.name for module in modules}
    kips = {module.name for module in modules if module.kind == "kip"}
    programs = [module for module in modules if module.kind == "program"]
    warnings = []
    if "sm" not in kips or "pm" not in kips:
        warnings.append("without sm and pm no service can be reached")
    if "fs" not in kips and ("pm" in kips):
        warnings.append("pm needs fs: sm holds back service lookups until pm has registered with fs")
    if programs and not {"spl", "ncm", "loader"} <= kips:
        warnings.append("programs are listed, but starting them needs spl, ncm and loader")
    fs_clients = {"fs_test", "content_fs_test", "content_test"} & names
    if fs_clients and ("loader" in names or len(fs_clients) > 1):
        warnings.append("fs_test, content_fs_test, content_test and the loader compete for the single fsp-ldr session")
    boot = [module for module in modules if "boot" in module.options]
    for module in boot:
        if module.kind != "program":
            fail(f"module '{module.source}' is a KIP: the kernel starts it, so 'boot' does not apply")
    if boot and "launcher" not in kips:
        warnings.append("programs are marked 'boot', but nothing starts them without the launcher module")
    if "launcher" in kips and "launch_test" in kips:
        warnings.append("launcher and launch_test would both start hello_program")
    for warning in warnings:
        print(f"build-os: warning: {warning}")


def main():
    parser = argparse.ArgumentParser(description="Build the whole OS and package it as one bootable disk image.")
    parser.add_argument("config", nargs="?", default=os.path.join(ROOT, "os.conf"), help="configuration file (default os.conf)")
    parser.add_argument("--out", help="output directory (default build/os/<configuration name>)")
    parser.add_argument("--list", action="store_true", help="show what would be built, and stop")
    parser.add_argument("--no-kernel", action="store_true", help="do not rebuild the bootloader and kernel")
    parser.add_argument("--no-modules", action="store_true", help="do not rebuild the modules; package what is already built")
    args = parser.parse_args()

    modules, settings = read_configuration(args.config)
    name = os.path.splitext(os.path.basename(args.config))[0]
    if name == "os":
        name = "default"
    out = os.path.abspath(args.out or os.path.join(ROOT, "build", "os", name))
    image = os.path.join(out, "disk.img")

    kips = [module for module in modules if module.kind == "kip"]
    programs = [module for module in modules if module.kind == "program"]
    print(f"configuration: {os.path.relpath(args.config, ROOT)}")
    for module in kips:
        print(f"  kip      {module.source}")
    for module in programs:
        print(f"  program  {module.source}  (id {module.program_id():016x}{', started at boot' if 'boot' in module.options else ''})")
    check_dependencies(modules)
    if args.list:
        return

    if not os.environ.get("DEVKITPRO"):
        fail("set DEVKITPRO to the devkitPro installation directory")
    os.environ.setdefault("DEVKITA64", os.path.join(os.environ["DEVKITPRO"], "devkitA64"))
    os.environ["PATH"] = os.pathsep.join([os.path.join(os.environ["DEVKITPRO"], "tools", "bin"),
                                          os.path.join(os.environ["DEVKITA64"], "bin"), os.environ["PATH"]])

    # 1. Patches. The kernel set comes with `make kernel`; the userland set is separate.
    run(["make", "setup-sysmodules"])

    # 2. Bootloader and kernel.
    if not args.no_kernel:
        run(["make", "bootloader"])
        run(["make", "kernel"])
    for required in ("build/BOOTAA64.EFI", "build/kernel.bin"):
        if not os.path.isfile(os.path.join(ROOT, required)):
            fail(f"{required} is missing; run without --no-kernel")

    # 3. Modules. libstratosphere is built by the first module that needs it if it does not exist yet.
    if not args.no_modules:
        for module in modules:
            module.build(checked_libstratosphere=os.path.isfile(LIBSTRATOSPHERE))

    # 4. Collect the outputs.
    shutil.rmtree(out, ignore_errors=True)
    kip_directory = os.path.join(out, "kips")
    os.makedirs(kip_directory)
    for module in kips:
        if not os.path.isfile(module.output("kip")):
            fail(f"module '{module.source}' did not produce {os.path.relpath(module.output('kip'), ROOT)}")
        shutil.copyfile(module.output("kip"), os.path.join(kip_directory, f"{module.name}.kip"))

    nca_directory = os.path.join(out, "ncas")
    for module in programs:
        # The code directory the loader expects: the executable as "main", its descriptor as "main.npdm".
        code = os.path.join(out, "code", module.name)
        os.makedirs(code)
        for extension, target in (("nso", "main"), ("npdm", "main.npdm")):
            if not os.path.isfile(module.output(extension)):
                fail(f"module '{module.source}' did not produce {os.path.relpath(module.output(extension), ROOT)}")
            shutil.copyfile(module.output(extension), os.path.join(code, target))
        run([sys.executable, "scripts/hznca.py", "program", "--id", hex(module.program_id()),
             "--version", module.options.get("version", "0"), "--code", code, "--out", nca_directory])

    # 5. The disk image: boot partition with the KIPs, then the programs into SYSTEM's content storage.
    environment = dict(os.environ, KIPS=kip_directory, LAYOUT=settings["layout"])
    run(["scripts/build-system-disk.sh", image, settings["size"]], env=environment)
    if programs:
        run([sys.executable, "scripts/hznca.py", "install", image, nca_directory])

    # 6. The list the launcher reads: the programs marked "boot", in configuration order.
    boot = [module for module in programs if "boot" in module.options]
    if boot:
        stage = os.path.join(out, "system")
        os.makedirs(stage)
        with open(os.path.join(stage, "boot.lst"), "w") as f:
            f.write("# Programs started at boot by the launcher, in order. Written by scripts/build-os.py.\n")
            for module in boot:
                f.write(f"{module.program_id():016x}  # {module.source}\n")
        run([sys.executable, "scripts/hzdisk.py", "put", image, "SYSTEM", stage, "/"], stdout=subprocess.DEVNULL)

    print(f"\nbuilt {os.path.relpath(image, ROOT)}")
    print(f"  boot it in QEMU:  scripts/run-os.sh {os.path.relpath(image, ROOT)}")


if __name__ == "__main__":
    main()
