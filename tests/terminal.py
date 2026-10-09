"""Exercise the actual terminal loop, including split rendering and resize events."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata


class Terminal:
    def __init__(self, rows, cols):
        self.resize(rows, cols)

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.backgrounds = [[None] * cols for _ in range(rows)]
        self.foregrounds = [[None] * cols for _ in range(rows)]
        self.background = None
        self.foreground = None
        self.row = self.col = 0
        self.cursor_visible = False

    def feed(self, data):
        text = data.decode("utf-8")
        index = 0
        while index < len(text):
            if text[index] == "\x1b":
                match = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", text[index:])
                assert match, repr(text[index:index + 20])
                parameters, command = match.groups()
                if command == "H":
                    parts = [int(p) if p else 1 for p in parameters.split(";")]
                    self.row = (parts[0] if parameters else 1) - 1
                    self.col = (parts[1] if len(parts) > 1 else 1) - 1
                    assert 0 <= self.row < self.rows and 0 <= self.col < self.cols
                elif command == "J" and parameters == "2":
                    self.grid = [[" "] * self.cols for _ in range(self.rows)]
                    self.backgrounds = [[None] * self.cols for _ in range(self.rows)]
                    self.foregrounds = [[None] * self.cols for _ in range(self.rows)]
                elif command == "m":
                    values = [int(p) if p else 0 for p in parameters.split(";")]
                    i = 0
                    while i < len(values):
                        value = values[i]
                        if value in (0, 49):
                            self.background = None
                        if value in (0, 39):
                            self.foreground = None
                        if 30 <= value <= 37 or 90 <= value <= 97:
                            self.foreground = value
                        elif 40 <= value <= 47 or 100 <= value <= 107:
                            self.background = value
                        elif value in (38, 48) and i + 2 < len(values) and values[i + 1] == 5:
                            if value == 48:
                                self.background = values[i + 2]
                            else:
                                self.foreground = values[i + 2]
                            i += 2
                        i += 1
                elif command in ("h", "l") and parameters == "?25":
                    self.cursor_visible = command == "h"
                index += len(match[0])
                continue
            character = text[index]
            assert 0 <= self.row < self.rows and 0 <= self.col < self.cols, (self.rows, self.cols, self.row, self.col, repr(text[max(0, index - 40):index + 40]))
            if unicodedata.combining(character):
                if self.col:
                    self.grid[self.row][self.col - 1] += character
            else:
                self.grid[self.row][self.col] = character
                self.backgrounds[self.row][self.col] = self.background
                self.foregrounds[self.row][self.col] = self.foreground
                width = 2 if unicodedata.east_asian_width(character) in "WF" else 1
                if width == 2:
                    assert self.col + 1 < self.cols
                    self.grid[self.row][self.col + 1] = ""
                    self.backgrounds[self.row][self.col + 1] = self.background
                    self.foregrounds[self.row][self.col + 1] = self.foreground
                self.col += width
            index += 1

    def lines(self):
        return ["".join(row) for row in self.grid]


def read_frame(fd):
    chunks = []
    deadline = time.monotonic() + 0.18
    while time.monotonic() < deadline:
        if select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
            try:
                data = os.read(fd, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks)


def wait_exit(process, fd, timeout=3):
    # TCSAFLUSH on macOS waits for output to drain. Keep reading the PTY
    # while waiting for exit, including the final screen cleanup sequence.
    chunks = []
    deadline = time.monotonic() + timeout
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout, output=b"".join(chunks))
        if not select.select([fd], [], [], min(remaining, 0.05))[0]:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            data = b""
        if not data:
            process.wait(timeout=max(0, deadline - time.monotonic()))
            break
        chunks.append(data)
    chunks.append(read_frame(fd))
    output = b"".join(chunks)
    assert process.returncode == 0, (process.args, process.returncode, output)
    return output


def run_exit_drain():
    master, slave = pty.openpty()
    # Exceed the PTY buffer so waiting without reading must block, even on Linux.
    payload = b"x" * 262144 + b"\x1b[?25h"
    process = subprocess.Popen(
        [sys.executable, "-c", "import sys; sys.stdout.buffer.write(b'x' * 262144 + b'\\x1b[?25h')"],
        stdout=slave, stderr=slave)
    os.close(slave)
    try:
        assert wait_exit(process, master) == payload
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
    print("Terminal exit-drain regression passed.")


def run(binary):
    with tempfile.TemporaryDirectory(prefix="adm-terminal-") as directory:
        filename = Path(directory) / "doc.txt"
        original = "alpha è 界\n" + "".join(f"line {i:02d}\n" for i in range(1, 70))
        filename.write_text(original)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        alias = Path(directory) / "alias.txt"
        alias.symlink_to(filename.name)
        process = subprocess.Popen([str(binary), alias.name], cwd=directory,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 80)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def resize(rows, cols):
            read_frame(master)  # Drain output at the old size before resizing.
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            assert " [C-x] " in update()
            initial_menu = update(b"\x18")
            assert "][ adm - Command Center" in initial_menu
            assert "Move between splits" in initial_menu and "Navigation help" in initial_menu
            assert "Next / Previous pane" not in initial_menu and not terminal.cursor_visible
            save_row = next(i for i, row in enumerate(terminal.lines()) if " - Save" in row)
            assert not terminal.lines()[save_row - 1].split("|")[1].strip()
            horizontal = update(b"q")
            assert " FILES *" in horizontal and "* 2  [Empty]" in horizontal
            # Open the same document into each new empty split.
            shared = update(b"\x1b[H\x1b[B\x1b[B\r")
            assert shared.count("alpha è 界") == 2 and terminal.cursor_visible
            update(b"\x15\r")
            mixed = update(b"\x11\r")
            assert mixed.count("alpha è 界") == 4
            popup = update(b"\x18")
            assert "Move between splits" in popup and "Close current split" in popup
            assert "} / {" in popup and "Grow / Shrink width" in popup
            assert "Split side by side" not in popup and not terminal.cursor_visible
            moved = update(b"\x07\x18m")
            assert " MOVE " in moved and not terminal.cursor_visible
            assert "h/j/k/l: split" in terminal.lines()[-1] and "b: sidebar" in terminal.lines()[-1]
            resize(24, 40)
            assert "hjkl:split" in terminal.lines()[-1] and "b:sidebar  Esc" in terminal.lines()[-1]
            resize(24, 80)
            unchanged = update(b"Xh")
            assert "Xalpha" not in unchanged and "* 2  doc.txt" in unchanged
            update(b"\x1b")
            update(b"Z\x0cQ")  # C-l moves right, sharing the modified text.
            assert update(b"\x0a").count("ZQalpha") == 4
            update(b"\x18\x13")
            assert filename.read_text() == "ZQ" + original
            update(b"\x16")
            assert terminal.cursor_visible and update().count("ZQalpha") == 3
            compact = resize(12, 40)
            assert compact.count("doc.txt") == 3
            menu = update(b"\x18")
            assert "Up/Down scroll" in menu
            menu = update(b"\x1b[6~\x1b[6~")
            assert "Save" in menu and "Quit" in menu
            update(b"\x07")
            resize(24, 80)
            update(b"\x18c\x18c\x18c\x1b<")
            assert update().count("ZQalpha") == 1 and terminal.cursor_visible
            tabbed = update(b"\x14")
            assert "[2/2]" in tabbed and "[Empty]" in tabbed and " FILES *" in tabbed
            update(b"\x18u\r")
            shared_tab = update(b"\x08\x02\r")  # Left empty pane, sidebar, open.
            assert shared_tab.count("ZQalpha") == 2
            original_tab = update(b"\x10")
            assert "[1/2]" in original_tab and original_tab.count("ZQalpha") == 1
            shared_tab = update(b"\x1b<Y\x0e\x1b<")
            assert "[2/2]" in shared_tab and "YZQalpha" in shared_tab
            narrow = resize(8, 14)
            assert "[2/2]" in narrow
            resize(24, 80)
            closed_tab = update(b"\x18k")
            assert "Close tab without saving" not in closed_tab and closed_tab.count("YZQalpha") == 1, closed_tab
            help_text = update(b"\x18?")
            assert "Navigation help" in help_text and "C-h C-j C-k C-l C-b C-n C-p" in help_text
            assert " HELP " in help_text and not terminal.cursor_visible
            update(b"\x1b[6~")
            assert "C-x k" in update()
            update(b"\x1b")
            warning = update(b"!\x18\x03")
            assert "Quit without saving? y=yes / any=no" in warning
            cancelled = update(b"n")
            assert "Quit without saving?" not in cancelled
            assert filename.read_text() == "ZQ" + original
            update(b"\x18\x03")
            os.write(master, b"y")
            wait_exit(process, master)
            assert alias.is_symlink()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Terminal regressions passed.")


def run_empty(binary):
    with tempfile.TemporaryDirectory(prefix="adm-empty-terminal-") as directory:
        root = Path(directory)
        (root / "folder").mkdir()
        (root / "existing.txt").write_text("unchanged\n")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 120, 0, 0))
        process = subprocess.Popen([str(binary)], cwd=root,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(28, 120)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def resize(rows, cols):
            read_frame(master)  # Drain output at the old size before resizing.
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            empty = update()
            assert " FILES *" in empty and "C-x N: Create new file" in empty
            assert "unchanged" not in empty and not terminal.cursor_visible
            row = next(i for i, line in enumerate(terminal.lines()) if "____   ____" in line)
            column = terminal.lines()[row].index("____   ____")
            assert all(c == 93 for c in terminal.foregrounds[row][column:column + 11])
            assert all(c is None for c in terminal.foregrounds[row][column + 13:column + 36])
            assert 7 <= row <= 17 and column > 45
            prompt = update(b"\x18N" + "long è file.txt".encode())
            assert " NEW FILE " in prompt and terminal.cursor_visible
            tiny = resize(5, 9)
            assert "N: " in tiny and terminal.cursor_visible
            assert terminal.col < 9
            resize(1, 1)
            assert terminal.cursor_visible and terminal.col == 0
            restored = resize(28, 120)
            assert "long è file.txt" in restored and terminal.cursor_visible
            cancelled = update(b"\x07")
            assert " FILES *" in cancelled and not terminal.cursor_visible
            assert not (root / "long è file.txt").exists()
            created = update(b"\x18N" + "è.txt".encode() + b"\r")
            assert "è.txt" in created and " FILES *" not in created and terminal.cursor_visible
            assert (root / "è.txt").read_bytes() == b""
            update(b"hello\x18\x13")
            assert (root / "è.txt").read_text() == "hello\n"
            tabbed = update(b"\x18t")
            assert "[2/2]" in tabbed and "[Empty]" in tabbed and " FILES *" in tabbed
            # A simple name uses the selected Explorer folder.
            update(b"\x1b[H\x1b[B\x18Nnested.txt\r")
            assert (root / "folder" / "nested.txt").read_bytes() == b""
            update(b"\x18t\x1b[H\x1b[B\x18N")
            absolute = root / "absolute.txt"
            update(str(absolute).encode() + b"\r")
            assert absolute.exists() and not (root / "folder" / "absolute.txt").exists()
            update(b"\x18t\x18Nexisting.txt\r")
            warning = update()
            assert "already exists" in warning and " NEW FILE " in warning
            assert (root / "existing.txt").read_text() == "unchanged\n"
            update(b"\x07\x18Nrelative/path.txt\r")
            assert "absolute path" in update()
            update(b"\x07\x18k")
            closed = update(b"\x18\x03")
            assert "Quit without saving" not in closed
            wait_exit(process, master)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Empty-view terminal regressions passed.")


def run_file_operations(binary):
    with tempfile.TemporaryDirectory(prefix="adm-file-operations-terminal-") as directory:
        root = Path(directory)
        original = root / "alpha.c"
        original.write_text("int value = 42;\n")
        (root / "zeta.txt").write_text("keep\n")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        process = subprocess.Popen([str(binary), original.name], cwd=root,
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env=dict(os.environ, ADM_CONFIG=str(root / "no-config")))
        os.close(slave)
        terminal = Terminal(24, 100)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def resize(rows, cols):
            read_frame(master)
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            update()
            update(b"!\x18f\x1b[B")
            text = update(b"rd\x1b[3~")
            assert " RENAME " not in text and "Delete '" not in text and original.exists()
            menu = update(b"\x18")
            for _ in range(3):
                menu += update(b"\x1b[6~")
            assert "Rename selected file" in menu and "Delete selected file" in menu
            update(b"\x1b")
            prompt = update(b"\x18n")
            assert " RENAME alpha.c" in prompt and terminal.cursor_visible
            # Unix must keep its original blue active palette and yellow status.
            assert terminal.backgrounds[1][0] == 44 and terminal.foregrounds[1][0] == 97
            assert terminal.backgrounds[0][0] == 103
            assert "R: " in resize(5, 9) and terminal.col < 9
            assert " RENAME " in resize(24, 100)
            renamed = root / "renamed è.py"
            text = update(b"\x1b[H" + b"\x1b[3~" * 20 + renamed.name.encode() + b"\r")
            assert renamed.name in text and "!int value = 42;" in text
            assert not original.exists() and renamed.read_text() == "int value = 42;\n"
            update(b"\x0c\x18\x13")
            assert renamed.read_text() == "!int value = 42;\n" and not original.exists()
            prompt = update(b"\x18f\x18n\x1b[H" + b"\x1b[3~" * 20 + b"zeta.txt\r")
            assert "Cannot rename" in prompt and " RENAME " in prompt
            assert (root / "zeta.txt").read_text() == "keep\n"
            update(b"\x1b")
            assert "Delete" in update(b"\x18d")
            update(b"n")
            assert renamed.exists()
            empty = update(b"\x18dy")
            assert not renamed.exists() and "[Empty]" in empty
            assert (root / "zeta.txt").read_text() == "keep\n"
            update(b"\x18\x03")
            wait_exit(process, master)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("File-operation terminal regressions passed.")


def run_file_manager(binary):
    with tempfile.TemporaryDirectory(prefix="adm-tree-terminal-") as directory:
        root = Path(directory)
        (root / "folder").mkdir()
        (root / "folder" / "nested.txt").write_text("inside folder\n")
        (root / "alpha.txt").write_text("alpha text\n")
        (root / "beta.txt").write_text("beta text\n")
        (root / "nul.bin").write_bytes(b"before\0after")
        (root / "界è.txt").write_text("unicode text\n")
        (root / "odd\x1b[2J.txt").write_text("odd filename\n")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        process = subprocess.Popen([str(binary), "alpha.txt"], cwd=directory,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 100)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def resize(rows, cols):
            read_frame(master)  # Drain output at the old size before resizing.
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            update()
            popup = update(b"\x18")
            assert "Open File Explorer" in popup and "Move between splits" in popup
            tree = update(b"f")
            assert " FILES *" in tree
            assert "alpha text" in tree and "界è.txt" in tree
            assert "odd?[2J.txt" in tree
            assert all(row[30] == "|" for row in terminal.grid[1:-1])
            assert not terminal.cursor_visible
            split = update(b"\x18u")
            assert " FILES *" in split and "* 2  [Empty]" in split
            assert not terminal.cursor_visible
            # Root, folder, alpha, beta: opening beta replaces only the selected right pane.
            opened = update(b"\x1b[B\x1b[B\x1b[BX\r")
            assert "beta text" in opened and "alpha text" in opened
            assert "* 2  beta.txt" in opened and "Xalpha" not in opened
            assert " FILES *" not in opened and terminal.cursor_visible
            dirty = update(b"B")
            assert "Bbeta text" in dirty
            # Reopen beta from the left pane: reuse its modified document, without confirmation.
            shared = update(b"\x08\x02\r")
            assert shared.count("Bbeta text") == 2 and "Replace unsaved buffer" not in shared
            # Restore alpha to the left pane, then attempt to replace the last modified beta view.
            update(b"\x02\x1b[A\r\x0c\x02\x1b[B\x1b[B\r")
            warning = update()
            assert "Replace unsaved buffer? y=yes / any=no" in warning
            cancelled = update(b"n")
            assert "Bbeta text" in cancelled and " FILES *" in cancelled
            update(b"\r")
            failed = update(b"y")
            assert "Cannot open file" in failed and "Bbeta text" in failed
            assert " FILES *" in failed
            # Open folder/nested in the same selected pane, confirm the last dirty view replacement.
            update(b"\x1b[H\x1b[B\x06\x06\r")
            opened = update(b"y")
            assert "inside folder" in opened and "* 2  nested.txt" in opened
            assert "alpha text" in opened and " FILES *" not in opened
            changed = update(b"N\x08A")
            assert "Ninside folder" in changed and "Aalpha text" in changed
            hidden = update(b"\x18f")
            assert " FILES *" in hidden
            restored_focus = update(b"\x02")
            assert " FILES *" in restored_focus and "* 1  alpha.txt" in restored_focus
            editor_focus = update(b"\x0c")
            assert " FILES *" not in editor_focus and " FILES" in editor_focus
            assert terminal.cursor_visible
            compact = resize(8, 20)
            assert "FILES" not in compact and terminal.cursor_visible
            restored = resize(24, 100)
            assert " FILES" in restored and "Ninside folder" in restored
            warning = update(b"\x18\x03")
            assert "Quit without saving?" in warning
            cancelled = update(b"n")
            assert "Aalpha text" in cancelled and "Ninside folder" in cancelled
            update(b"\x18\x03")
            os.write(master, b"y")
            wait_exit(process, master)
            assert (root / "alpha.txt").read_text() == "alpha text\n"
            assert (root / "beta.txt").read_text() == "beta text\n"
            assert (root / "folder" / "nested.txt").read_text() == "inside folder\n"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("File-manager terminal regressions passed.")


def run_git(binary, create_repository):
    with tempfile.TemporaryDirectory(prefix="adm-git-terminal-") as directory:
        root, remote, environment, git = create_repository(Path(directory))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 120, 0, 0))
        process = subprocess.Popen([str(binary), "alpha.txt"], cwd=root, env=environment,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(28, 120)
        captured = bytearray()

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            frame = read_frame(master)
            captured.extend(frame)
            terminal.feed(frame)
            return "\n".join(terminal.lines())

        def ready(predicate=lambda text: "working..." not in text):
            for _ in range(60):
                text = update()
                if predicate(text):
                    return text
                assert process.poll() is None, text
            raise AssertionError("Git UI did not settle:\n" + text)

        def resize(rows, cols):
            read_frame(master)  # Drain output at the old size before resizing.
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            update()
            files = update(b"\x18f")
            assert " FILES *" in files and "alpha change" in files
            editor_focus = update(b"\x0c")
            assert " FILES *" not in editor_focus and terminal.cursor_visible
            update(b"\x18g")
            panel = ready(lambda text: "Staged (2)" in text and "working..." not in text)
            assert " FILES" not in panel and " GIT *" in panel
            assert "s/u stage/unstage" in panel and "S/U stage/unstage all" in panel
            assert "p/P pull/push" in panel and "b checkout  m merge" in panel
            assert "line?break.txt" in panel and panel.count("dual.txt") == 2
            assert all(row[40] == "|" for row in terminal.grid[1:-1])
            assert not terminal.cursor_visible
            update(b"X\r")
            diff = ready(lambda text: "Diff [staged]: dual.txt" in text and "working..." not in text)
            assert " DIFF " in diff and "+staged" in diff and "-base" in diff
            for text, background in (("+staged", 22), ("-base", 52), (" before", 235)):
                row = next(i for i, line in enumerate(terminal.lines()) if text in line)
                assert all(colour == background for colour in terminal.backgrounds[row][41:])
                assert terminal.backgrounds[row][40] is None
            header = next(i for i, line in enumerate(terminal.lines()) if "+++ b/dual.txt" in line)
            assert all(colour is None for colour in terminal.backgrounds[header][41:])
            added_row = next(i for i, line in enumerate(terminal.lines()) if "+staged" in line)
            update(b"\x18l" + str(added_row).encode() + b"\r\x01\x00\x06")
            assert terminal.backgrounds[added_row][43] == 44
            assert all(colour == 22 for colour in terminal.backgrounds[added_row][44:])
            update(b"\x07")
            assert all(colour == 22 for colour in terminal.backgrounds[added_row][41:])
            assert terminal.cursor_visible
            unchanged = update(b"X\x04\x7f\x19")
            assert "Xdiff" not in unchanged and " **" not in terminal.lines()[0]
            tree = update(b"\x18f")
            assert " FILES" in tree and " GIT" not in tree
            assert "Diff [staged]: dual.txt" in tree
            update(b"\x18g")
            panel = ready()
            assert " GIT *" in panel and not terminal.cursor_visible
            update(b"S")
            staged = ready(lambda text: "Changes (0)" in text and "working..." not in text)
            assert "Staged (" in staged
            prompt = update(b"c")
            assert "Commit message:" in prompt and terminal.cursor_visible
            text = "Terminal commit 'è' $(touch ignored) with a sufficiently long message"
            update(text.encode())
            assert terminal.col < 40 and terminal.cursor_visible
            update(b"\x01")
            assert terminal.col == 0
            update(b"\x05\r")
            clean = ready(lambda text: "Working tree clean" in text and "working..." not in text)
            assert "Staged (0)" in clean and "Changes (0)" in clean
            assert not (root / "ignored").exists()
            assert git(root, "log", "-1", "--format=%s").decode().strip() == text
            update(b"b")
            checkout = ready(lambda text: "New branch..." in text and "working..." not in text)
            assert "Enter checkout" in checkout and "feature" in checkout
            branch_prompt = update(b"\r")
            assert "New branch name:" in branch_prompt and terminal.cursor_visible
            update(b"terminal-branch\r")
            ready(lambda text: "terminal-branch" in text and "working..." not in text)
            assert git(root, "branch", "--show-current").strip() == b"terminal-branch"
            update(b"m")
            merge = ready(lambda text: "Enter merge" in text and "working..." not in text)
            assert "feature" in merge and "C-g back" in merge
            update(b"\x07")
            hidden = update(b"\x18g")
            assert " GIT *" in hidden and " FILES" not in hidden
            update(b"\x02")
            restored_focus = ready()
            assert " GIT *" in restored_focus and " FILES" not in restored_focus
            editor_focus = update(b"\x0c")
            assert " GIT *" not in editor_focus and " GIT" in editor_focus
            assert terminal.cursor_visible
            update(b"\x02")
            ready()
            compact = resize(8, 20)
            assert " GIT" not in compact and terminal.cursor_visible
            restored = resize(28, 120)
            assert " GIT" in restored and "s/u stage/unstage" in restored
            update(b"\x18\x03")
            wait_exit(process, master)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Git terminal regressions passed.")


if __name__ == "__main__":
    run(Path(__file__).resolve().parent.parent / "adm")
    run_file_manager(Path(__file__).resolve().parent.parent / "adm")


def run_search(binary):
    with tempfile.TemporaryDirectory(prefix="adm-search-terminal-") as directory:
        root = Path(directory)
        (root / "nested").mkdir()
        (root / "doc.txt").write_text("è needle needle\na.c abc\n")
        (root / "nested" / "other.txt").write_text("needle\n")
        (root / "ignored.bin").write_bytes(b"needle\0")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        process = subprocess.Popen([str(binary), "doc.txt"], cwd=root,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 100)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def ready(fragment):
            text = update()
            deadline = time.monotonic() + 5
            while fragment not in text and time.monotonic() < deadline:
                text = update()
            assert fragment in text, text
            return text

        def resize(rows, cols):
            read_frame(master)  # Drain output at the old size before resizing.
            terminal.resize(rows, cols)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            update()
            update(b"\x13needle")
            ready("1/2")
            assert "SEARCH FILE TEXT" in terminal.lines()[-1] and terminal.cursor_visible
            update(b"\x1b[B")
            assert "2/2" in terminal.lines()[-1]
            update(b"\x07\x13" + "è".encode())
            ready("1/1")
            resize(6, 20)
            assert "FIND TXT" in terminal.lines()[-1] and terminal.cursor_visible
            resize(1, 1)
            assert terminal.cursor_visible and terminal.col == 0
            resize(24, 100)
            update(b"\x07\x18sa.c")
            ready("1/1")
            update(b"\t")
            ready("1/2")
            assert "FILE REGEX" in terminal.lines()[-1]
            update(b"\x07\x18Sneedle")
            ready("1/3")
            assert "FILES * SEARCH" in update() and "ignored.bin" not in update()
            update(b"\r")
            assert "Up/Down: hits Enter:open" in terminal.lines()[-1]
            update(b"\x02")
            # Parallel files arrive in completion order; select by name.
            for _ in range(2):
                if any("other.txt" in row and terminal.backgrounds[i][0] == 44
                       for i, row in enumerate(terminal.lines())):
                    break
                update(b"\x1b[B")
            else:
                raise AssertionError("Cannot select other.txt")
            update(b"\r\x07")
            update(b"\x18rneedle")
            ready("1/1")
            update(b"\rvalue\r")
            assert "y:replace n:next a:all" in terminal.lines()[-1]
            update(b"y")
            ready("value")
            update(b"\x07\x18\x13")
            assert (root / "nested" / "other.txt").read_text() == "value\n"
            update(b"\x18Rneedle")
            ready("1/2")
            update(b"\rword\ra")
            assert "Replace 2 matches in 1 workspace files" in update()
            update(b"y")
            ready("Replacement finished")
            assert (root / "doc.txt").read_text() == "è word word\na.c abc\n"
            update(b"\x18\x03")
            wait_exit(process, master)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Search terminal regressions passed.")


def run_modal(binary):
    with tempfile.TemporaryDirectory(prefix="adm-modal-terminal-") as directory:
        root = Path(directory)
        filename = root / "doc.c"
        original = "needle needle\nsecond\n"
        filename.write_text(original)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        environment = dict(os.environ, ADM_CONFIG=str(root / "no-config"))
        process = subprocess.Popen([str(binary), filename.name], cwd=root, env=environment,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 100)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        try:
            update()
            assert "SEARCH FILE TEXT" in update(b"\x13needle")
            assert "SEARCH FILE TEXT" not in update(b"\x1b")
            assert "Command Center" in update(b"\x13needle\x18")
            assert "LSP  M-x" in update(b"\x1bx")
            assert "Command Center" in update(b"\x18")
            update(b"\x1b")
            assert "LSP  M-x" in update(b"\x18Sneedle\x1bx")
            update(b"\x1b")
            assert "RENAME New name" in update(b"\x1bxN")
            assert "Command Center" in update(b"name\x18")
            assert "Navigation help" in update(b"?")
            assert "LSP  M-x" in update(b"\x1bx")
            update(b"\x1b")
            update(b"\x18m")
            assert "Command Center" in update(b"\x18")
            update(b"\x1b")
            assert "USER CENTER" in update(b"\x1bc")
            assert "Command Center" in update(b"\x18")
            update(b"\x1b")
            assert filename.read_text() == original
            # A dirty buffer still needs confirmation when quitting from search.
            update(b"!\x13needle")
            assert "Quit without saving?" in update(b"\x18\x03")
            assert "Quit without saving?" not in update(b"\x1b")
            assert process.poll() is None
            update(b"\x13needle\x18\x03y")
            wait_exit(process, master)
            assert filename.read_text() == original
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Global modal-key terminal regressions passed.")


def run_undo(binary):
    with tempfile.TemporaryDirectory(prefix="adm-undo-terminal-") as directory:
        filename = Path(directory) / "doc.txt"
        filename.write_text("base\n")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        process = subprocess.Popen([str(binary), filename.name], cwd=directory,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 100)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        try:
            update()
            assert "abc base" in update(b"abc ")
            assert "abc defbase" in update(b"def")
            assert "abc base" in update(b"\x1a")  # C-z must not suspend adm.
            assert "abcbase" in update(b"\x1a")
            clean = update(b"\x1a")
            assert "base" in clean and " **" not in terminal.lines()[0]
            assert "abc defbase" in update(b"\x1bz\x1bz\x1bz")
            update(b"\x18\x13")
            assert filename.read_text() == "abc defbase\n"
            assert " **" not in terminal.lines()[0]
            assert "abc base" in update(b"\x18z")
            assert " **" in terminal.lines()[0]
            assert "abc defbase" in update(b"\x18Z")
            assert " **" not in terminal.lines()[0]
            update(b"\x15")  # New vertical split; open the same file.
            shared = update(b"\x1b[B\r")
            assert shared.count("abc defbase") == 2, shared
            restored = update(b"\x1a")
            assert restored.count("abc base") == 2
            assert update(b"\x1bz").count("abc defbase") == 2
            update(b"\x18\x03")
            wait_exit(process, master)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Undo terminal regressions passed.")


def run_lsp(binary):
    with tempfile.TemporaryDirectory(prefix="adm-lsp-terminal-") as directory:
        filename = Path(directory) / "doc.c"
        original = 'int value = 42; // comment\nchar *text = "hello";\n'
        filename.write_text(original)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        environment = dict(os.environ, ADM_LSP_CLANGD=str(Path(directory) / "missing-server"))
        process = subprocess.Popen([str(binary), filename.name], cwd=directory,
                                   stdin=slave, stdout=slave, stderr=slave, env=environment)
        os.close(slave)
        terminal = Terminal(24, 80)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def coloured(kind):
            return any(kind in row for row in terminal.foregrounds[1:-1])

        try:
            text = update()
            assert "int value = 42" in text and coloured(96) and coloured(93) and coloured(90)
            menu = update(b"\x1bx")
            assert "LSP  M-x" in menu and "Rename symbol" in menu and "Format file" in menu
            assert not terminal.cursor_visible
            update(b"t")
            plain = update(b"\x1b")
            assert "int value = 42" in plain and not coloured(96) and not coloured(93)
            update(b"\x1bxt\x1b")
            assert coloured(96)
            update(b"\x1bxs")
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                menu = update(b"\x1bx")
                if "unavailable" in menu or "Cannot start" in menu:
                    break
                update(b"\x1b")
            assert ("unavailable" in menu or "Cannot start" in menu) and "int value = 42" == filename.read_text().split(';')[0]
            update(b"\x1b")
            update(b"\x1bxN")
            assert "RENAME New name" in terminal.lines()[-1] and terminal.cursor_visible
            update("nuovoè".encode())
            assert "nuovoè" in terminal.lines()[-1]
            update(b"\x1b[D\x7f")
            assert "nuovè" in terminal.lines()[-1]
            update(b"\x1b")
            assert "int value = 42" in update()
            for rows, cols in ((7, 24), (3, 8), (2, 3)):
                read_frame(master)
                terminal.resize(rows, cols)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                process.send_signal(signal.SIGWINCH)
                update()
                update(b"\x1bx")
                update(b"\x1b[B\x1b[B\x1b")
                update(b"\x1bxN")
                update("è界".encode())
                assert terminal.cursor_visible
                update(b"\x1b")
            update(b"\x18\x03")
            wait_exit(process, master)
            assert filename.read_text() == original
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("LSP terminal regressions passed.")
