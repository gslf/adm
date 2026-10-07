"""Compile and run control regressions without touching the desktop clipboard."""
import os
import json
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent

help_source = (root / "src" / "navigation.c").read_text().split(
    "static const char *help_lines[] = {", 1)[1].split("};", 1)[0]
help_lines = [json.loads(line) for line in re.findall(r'^\s*(".*"),?\s*$', help_source, re.M)]
help_reference = (root / "HELP.md").read_text().split("```text\n", 1)[1].split("\n```", 1)[0]
assert "\n".join(help_lines) == help_reference, "HELP.md must match the in-editor help"


def git_fixture(parent):
    repository = parent / "git repository"
    remote = parent / "git remote.git"
    client = parent / "git client"
    empty_config = parent / "empty-config"
    empty_config.write_text("")
    environment = dict(os.environ, GIT_CONFIG_GLOBAL=str(empty_config),
                       GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0")
    executable = shutil.which("git")
    assert executable, "Git is required for Git-panel regression tests"

    def git(directory, *args):
        return subprocess.run([executable, "-C", str(directory), *args],
                              env=environment, check=True, capture_output=True).stdout

    repository.mkdir()
    git(repository, "init", "-b", "main")
    for directory in (repository,):
        git(directory, "config", "user.name", "adm tests")
        git(directory, "config", "user.email", "tests@example.invalid")
        git(directory, "config", "commit.gpgsign", "false")
    for name, content in (("alpha.txt", "base\n"), ("dual.txt", "before\nbase\nafter\n"),
                          ("old.txt", "rename\n"), ("deleted.txt", "deleted\n"),
                          ("vanishing.txt", "vanishing\n")):
        (repository / name).write_text(content)
    git(repository, "add", "-A")
    git(repository, "commit", "-m", "Initial files")
    git(repository, "branch", "pull-target")
    git(repository, "switch", "-c", "feature")
    (repository / "alpha.txt").write_text("feature\n")
    (repository / "feature.txt").write_text("feature file\n")
    (repository / "vanishing.txt").unlink()
    git(repository, "add", "-A")
    git(repository, "commit", "-m", "Feature changes")
    git(repository, "switch", "main")
    subprocess.run([executable, "init", "--bare", "-b", "main", str(remote)],
                   env=environment, check=True, capture_output=True)
    git(repository, "remote", "add", "origin", str(remote))
    git(repository, "push", "-u", "origin", "main", "feature", "pull-target")
    subprocess.run([executable, "clone", "--branch", "pull-target", str(remote), str(client)],
                   env=environment, check=True, capture_output=True)
    git(client, "config", "user.name", "adm tests")
    git(client, "config", "user.email", "tests@example.invalid")
    git(client, "config", "commit.gpgsign", "false")
    (client / "remote-target.txt").write_text("remote content\n")
    git(client, "add", "-A")
    git(client, "commit", "-m", "Remote update")
    git(client, "push")
    git(repository, "fetch", "origin")
    (repository / "dual.txt").write_text("before\nstaged\nafter\n")
    git(repository, "add", "dual.txt")
    (repository / "dual.txt").write_text("before\nunstaged\nafter\n")
    (repository / "old.txt").rename(repository / "renamed file.txt")
    git(repository, "add", "--", "old.txt", "renamed file.txt")
    (repository / "alpha.txt").write_text("alpha change\n")
    (repository / "deleted.txt").unlink()
    for name in ("new file.txt", "literal[1].txt", "literal1.txt", "界.txt"):
        (repository / name).write_text("new content\n")
    if os.name == "posix":
        (repository / "line\nbreak.txt").write_text("newline filename\n")
        runtime_bin = parent / "git-bin"
        runtime_bin.mkdir()
        (runtime_bin / "git").symlink_to(executable)
        environment["PATH"] = str(runtime_bin)
    return repository, remote, environment, git


with tempfile.TemporaryDirectory(prefix="adm-controls-") as directory:
    sources = sorted(p for p in (root / "src").glob("*.c") if p.name != "main.c")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE"))
    if os.name == "posix" and "-pthread" not in flags:
        flags.append("-pthread")
    for suite in ("languages", "lsp", "lsp_edits", "pattern", "search", "undo", "controls", "navigation", "splits", "tabs", "empty_views", "large_files", "lifecycle", "file_manager", "process", "diff", "git", "git_initial"):
        binary = Path(directory) / suite
        subprocess.run(compiler + flags + (["-DADM_TEST_ALLOC"] if suite in ("undo", "lsp_edits") else []) + ["-I", str(root / "src"), "-o", str(binary),
                                         str(root / "tests" / f"{suite}.c")]
                       + [str(p) for p in sources], check=True)
        environment = dict(os.environ, PATH="")
        argument = Path(directory) / f"{suite}.txt"
        working_directory = root
        if suite == "git":
            working_directory, remote, environment, git = git_fixture(Path(directory))
            argument = working_directory / "alpha.txt"
        if suite == "git_initial":
            working_directory = Path(directory) / "initial repository"
            working_directory.mkdir()
            subprocess.run([shutil.which("git"), "-C", str(working_directory), "init", "-b", "main"],
                           check=True, capture_output=True)
            environment = dict(os.environ, GIT_CONFIG_GLOBAL=str(Path(directory) / "empty-config"),
                               GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0")
            for name, value in (("user.name", "adm tests"), ("user.email", "tests@example.invalid"),
                                ("commit.gpgsign", "false")):
                subprocess.run([shutil.which("git"), "-C", str(working_directory), "config", name, value],
                               env=environment, check=True)
            for name in ("alpha.txt", "beta.txt"):
                (working_directory / name).write_text("new content\n")
            if os.name == "posix":
                environment["PATH"] = str(Path(directory) / "git-bin")
                hook = working_directory / ".git" / "hooks" / "pre-commit"
                hook.write_text("#!/bin/sh\nprintf 'hook rejected\\n' >&2\nexit 1\n")
                hook.chmod(0o755)
            argument = working_directory / "alpha.txt"
        if suite == "search":
            working_directory = Path(directory) / "search workspace"
            working_directory.mkdir()
            (working_directory / "folder").mkdir()
            (working_directory / ".git").mkdir()
            (working_directory / "folder" / "nested.txt").write_bytes(b"needle\r\nneedle")
            (working_directory / ".hidden.txt").write_bytes(b"\xef\xbb\xbfneedle\n")
            (working_directory / ".git" / "metadata").write_text("needle\n")
            (working_directory / "unrelated.txt").write_text("nothing\n")
            (working_directory / "binary.bin").write_bytes(b"needle\n" + b"x" * 65536 + b"\0")
            if os.name == "posix":
                (working_directory / "folder" / "loop").symlink_to(working_directory, target_is_directory=True)
                os.mkfifo(working_directory / "pipe")
            argument = working_directory / "search.txt"
        if suite == "empty_views":
            working_directory = Path(directory) / "empty views"
            working_directory.mkdir()
            (working_directory / "folder").mkdir()
            (working_directory / "existing.txt").write_text("keep\n")
            argument = working_directory / "absolute.txt"
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
        if suite == "git":
            assert git(working_directory, "branch", "--show-current").strip() == b"panel-branch"
            assert git(remote, "show", "feature:alpha.txt") == b"resolved\n"
            assert b"Add tests $(touch should-not-exist) 'quotes'" in git(
                working_directory, "log", "--all", "--format=%s")
        print(f"{suite.capitalize()} regressions passed.")
    if os.name == "posix":
        from terminal import run, run_empty, run_file_manager, run_git, run_search, run_undo, run_lsp
        binary = Path(directory) / "adm"
        subprocess.run(compiler + flags + ["-o", str(binary), str(root / "src" / "main.c")]
                       + [str(p) for p in sources], check=True)
        run(binary)
        run_undo(binary)
        run_lsp(binary)
        run_search(binary)
        run_empty(binary)
        run_file_manager(binary)
        run_git(binary, git_fixture)
