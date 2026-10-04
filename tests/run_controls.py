"""Compile and run control regressions without touching the desktop clipboard."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="adm-controls-") as directory:
    sources = sorted(p for p in (root / "src").glob("*.c") if p.name != "main.c")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE"))
    for suite in ("controls", "splits", "lifecycle", "file_manager"):
        binary = Path(directory) / suite
        subprocess.run(compiler + flags + ["-I", str(root / "src"), "-o", str(binary),
                                         str(root / "tests" / f"{suite}.c")]
                       + [str(p) for p in sources], check=True)
        environment = dict(os.environ, PATH="")
        argument = Path(directory) / f"{suite}.txt"
        working_directory = root
        if suite == "file_manager":
            working_directory = Path(directory) / "files"
            working_directory.mkdir()
            (working_directory / "empty").mkdir()
            (working_directory / "folder").mkdir()
            (working_directory / "folder" / "nested.txt").write_text("nested\n")
            for name in ("alpha", "beta", "gamma", "gone"):
                (working_directory / f"{name}.txt").write_text(f"{name}\n")
            (working_directory / "nul.bin").write_bytes(b"before\0after")
            (working_directory / ".hidden").write_text("hidden\n")
            if os.name == "posix":
                (working_directory / "beta.link").symlink_to("beta.txt")
                os.mkfifo(working_directory / "pipe")
            argument = working_directory / "alpha.txt"
        subprocess.run([str(binary), str(argument)], env=environment,
                       cwd=working_directory, stdout=subprocess.PIPE, check=True)
        print(f"{suite.capitalize()} regressions passed.")
    if os.name == "posix":
        from terminal import run, run_file_manager
        binary = Path(directory) / "adm"
        subprocess.run(compiler + flags + ["-o", str(binary), str(root / "src" / "main.c")]
                       + [str(p) for p in sources], check=True)
        run(binary)
        run_file_manager(binary)
