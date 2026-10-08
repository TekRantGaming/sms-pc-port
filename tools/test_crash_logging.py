#!/usr/bin/env python3
"""Run the ROM-free crash probe and verify diagnostics and real exit statuses.

POSIX: pass a probe linked with the runtime's crash handlers and --gc-sections.
Windows: link platform/os/tests/crash_probe.cpp and os/windows_crash.cpp;
64-bit also links os/windows_stack.cpp. Run on Windows, not through MSYS bash.
"""
import argparse
import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", nargs="?")
parser.add_argument("--build-directory", help="Compile the POSIX probe using an existing game's compile_commands.json")
parser.add_argument("--arch", choices=("32", "64"), default="64")
args = parser.parse_args()
temporary = None
if args.build_directory:
    if sys.platform != "linux":
        parser.error("--build-directory is for Linux; pass a native probe executable on other platforms")
    build = pathlib.Path(args.build_directory)
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(command for command in commands if command["file"].endswith("/platform/port_runtime.cpp"))
    temporary = tempfile.TemporaryDirectory(prefix="sms-crash-probe-")
    folder = pathlib.Path(temporary.name)
    compile_args = shlex.split(entry["command"])
    compile_args[compile_args.index("-o") + 1] = str(folder / "runtime.o")
    compile_args.extend(["-ffunction-sections", "-fdata-sections"])
    subprocess.run(compile_args, cwd=entry["directory"], check=True)
    root = pathlib.Path(__file__).resolve().parent.parent
    args.probe = str(folder / "probe")
    subprocess.run([compile_args[0], "-m" + args.arch, "-std=c++11", "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-I" + str(root / "src"),
                    str(root / "platform/os/tests/crash_probe.cpp"), str(folder / "runtime.o"),
                    "-pthread", "-ldl", "-o", args.probe], check=True)
if not args.probe:
    parser.error("pass a probe executable or --build-directory")
if os.name == "nt":
    cases = [("access", 0xC0000005), ("heap", 0xC0000374)]
    if args.arch == "64":
        cases.append(("low-stack", 0xC0000005))
else:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    cases = [("access", signal.SIGSEGV), ("abort", signal.SIGABRT),
             ("thread", signal.SIGSEGV), ("overflow", signal.SIGSEGV)]
for mode, expected in cases:
    run = subprocess.run([args.probe, mode], capture_output=True, text=True, timeout=15)
    if os.name == "nt":
        assert run.returncode & 0xFFFFFFFF == expected, (mode, run.returncode, run.stderr)
        assert "Windows exception 0x%X" % expected in run.stderr, (mode, run.stderr)
    else:
        # macOS exits with 128+signal to avoid Rosetta's fatal-signal exit hang.
        assert run.returncode in (-expected, 128 + expected), (mode, run.returncode, run.stderr)
        assert "fatal signal " + signal.Signals(expected).name in run.stderr, (mode, run.stderr)
        assert re.search(r"pc 0x[1-9A-F][0-9A-F]*", run.stderr), (mode, run.stderr)
    assert re.search(r"exception module .* offset 0x[0-9a-fA-F]+", run.stderr), (mode, run.stderr)
    print("%s: correct status and crash address/module diagnostics" % mode)
if temporary:
    temporary.cleanup()
