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
                elif command in ("h", "l") and parameters == "?25":
                    self.cursor_visible = command == "h"
                index += len(match[0])
                continue
            character = text[index]
            assert 0 <= self.row < self.rows and 0 <= self.col < self.cols
            if unicodedata.combining(character):
                if self.col:
                    self.grid[self.row][self.col - 1] += character
            else:
                self.grid[self.row][self.col] = character
                width = 2 if unicodedata.east_asian_width(character) in "WF" else 1
                if width == 2:
                    assert self.col + 1 < self.cols
                    self.grid[self.row][self.col + 1] = ""
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
                chunks.append(os.read(fd, 65536))
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
    return b"".join(chunks)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="adm-terminal-") as directory:
        filename = Path(directory) / "doc.txt"
        original = "alpha è 界\n" + "".join(f"line {i:02d}\n" for i in range(1, 70))
        filename.write_text(original)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        process = subprocess.Popen([str(binary), filename.name], cwd=directory,
                                   stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        terminal = Terminal(24, 80)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def resize(rows, cols):
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            terminal.resize(rows, cols)
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            assert " [C-x] " in update()
            initial_menu = update(b"\x18")
            assert "][ adm - Command Center" in initial_menu
            assert "Split above / below" in initial_menu and "Next / Previous pane" not in initial_menu
            menu_lines = terminal.lines()
            standard_end = next(i for i, line in enumerate(menu_lines)
                                if "Focus file manager / editor" in line)
            contextual_start = next(i for i, line in enumerate(menu_lines)
                                    if "Split above / below" in line)
            assert contextual_start == standard_end + 2
            blank = menu_lines[standard_end + 1]
            assert blank[blank.index("|") + 1:blank.rindex("|")].strip() == ""
            assert not terminal.cursor_visible
            horizontal = update(b"2")
            assert horizontal.count("doc.txt") == 3 and "-" * 80 in horizontal
            vertical = update(b"\x183")
            assert vertical.count("doc.txt") == 4 and "|" in vertical
            mixed = update(b"\x18o\x182")
            assert mixed.count("doc.txt") == 5 and terminal.cursor_visible
            popup = update(b"\x18")
            assert "o / O - Next / Previous pane" in popup
            menu_lines = terminal.lines()
            standard_end = next(i for i, line in enumerate(menu_lines)
                                if "Focus file manager / editor" in line)
            contextual_start = next(i for i, line in enumerate(menu_lines)
                                    if "Next / Previous pane" in line)
            assert contextual_start == standard_end + 2
            blank = menu_lines[standard_end + 1]
            assert blank[blank.index("|") + 1:blank.rindex("|")].strip() == ""
            assert "} / { - Grow / Shrink width" in popup
            labels = ("Save", "Quit", "Next / Previous pane", "Grow / Shrink width")
            prefixes = [next(line[:line.index(label)] for line in terminal.lines()
                             if " - " + label in line) for label in labels]
            columns = [sum(0 if unicodedata.combining(c) else
                           2 if unicodedata.east_asian_width(c) in "WF" else 1
                           for c in prefix) for prefix in prefixes]
            assert len(set(columns)) == 1, (columns, terminal.lines())
            assert "Split side by side" not in popup
            assert "][ adm - Command Center" in popup
            assert not terminal.cursor_visible
            update(b"oZ\x18OQ")
            shared = update()
            assert shared.count("ZQalpha") == 4
            update(b"\x18\x13")
            assert filename.read_text() == "ZQ" + original
            update(b"\x16")
            assert terminal.cursor_visible
            # One pane scrolled, while the other views retained their own origins.
            assert update().count("ZQalpha") == 3
            compact = resize(12, 40)
            assert compact.count("doc.txt") == 2
            menu = update(b"\x18")
            assert "Up/Down scroll" in menu
            assert "Grow / Shrink width" not in menu
            menu = update(b"\x1b[6~")
            assert "Keep only this pane" in menu
            update(b"\x07")
            restored = resize(24, 80)
            assert restored.count("doc.txt") == 5
            resized = update(b"\x18}\x18]")
            assert resized.count("doc.txt") == 5 and terminal.cursor_visible
            closed = update(b"\x180")
            assert closed.count("doc.txt") == 4
            single = update(b"\x181")
            assert single.count("doc.txt") == 1 and terminal.cursor_visible
            popup = update(b"\x18")
            assert "Next / Previous pane" not in popup and "Grow / Shrink width" not in popup
            warning = update(b"\x07!\x18\x03")
            assert "Quit without saving? y=yes / any=no" in warning
            cancelled = update(b"n")
            assert "Quit without saving?" not in cancelled
            assert filename.read_text() == "ZQ" + original
            update(b"\x18\x03")
            cancelled = update(b"x")
            assert "Quit without saving?" not in cancelled
            warning = update(b"\x18\x03")
            assert "y=yes / any=no" in warning
            os.write(master, b"y")
            assert process.wait(timeout=3) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Terminal regressions passed.")


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
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            terminal.resize(rows, cols)
            process.send_signal(signal.SIGWINCH)
            return update()

        try:
            update()
            popup = update(b"\x18")
            assert "Toggle file manager" in popup and "Focus file manager / editor" in popup
            tree = update(b"t")
            assert " FILES" in tree and " FILES *" not in tree
            assert "alpha text" in tree and "界è.txt" in tree
            assert "odd?[2J.txt" in tree
            assert all(row[30] == "|" for row in terminal.grid[1:-1])
            assert terminal.cursor_visible
            split = update(b"\x183\x18o\x18f")
            assert " FILES *" in split and "* 2  alpha.txt" in split
            assert not terminal.cursor_visible
            # Root, folder, alpha, beta: opening beta replaces only the selected right pane.
            opened = update(b"\x0e\x0e\x0eX\r")
            assert "beta text" in opened and "alpha text" in opened
            assert "* 2  beta.txt" in opened and "Xalpha" not in opened
            assert " FILES *" not in opened and terminal.cursor_visible
            dirty = update(b"B")
            assert "Bbeta text" in dirty
            # Reopen beta from the left pane: reuse its modified document, without confirmation.
            shared = update(b"\x18o\x18f\r")
            assert shared.count("Bbeta text") == 2 and "Replace unsaved buffer" not in shared
            # Restore alpha to the left pane, then attempt to replace the last modified beta view.
            update(b"\x18f\x10\r\x18o\x18f\x0e\x0e\r")
            warning = update()
            assert "Replace unsaved buffer? y=yes / any=no" in warning
            cancelled = update(b"n")
            assert "Bbeta text" in cancelled and " FILES *" in cancelled
            update(b"\r")
            failed = update(b"y")
            assert "Cannot open file" in failed and "Bbeta text" in failed
            assert " FILES *" in failed
            # Open folder/nested in the same selected pane, confirm the last dirty view replacement.
            update(b"\x1b[H\x0e\x06\x06\r")
            opened = update(b"y")
            assert "inside folder" in opened and "* 2  nested.txt" in opened
            assert "alpha text" in opened and " FILES *" not in opened
            changed = update(b"N\x18oA")
            assert "Ninside folder" in changed and "Aalpha text" in changed
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
            assert process.wait(timeout=3) == 0
            assert (root / "alpha.txt").read_text() == "alpha text\n"
            assert (root / "beta.txt").read_text() == "beta text\n"
            assert (root / "folder" / "nested.txt").read_text() == "inside folder\n"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("File-manager terminal regressions passed.")


if __name__ == "__main__":
    run(Path(__file__).resolve().parent.parent / "adm")
    run_file_manager(Path(__file__).resolve().parent.parent / "adm")
