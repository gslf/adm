"""Exercise a configured theme and a real C plugin through the terminal loop."""
from terminal import Terminal, read_frame, wait_exit
import fcntl
import os
from pathlib import Path
import pty
import re
import signal
import struct
import subprocess
import tempfile
import termios
import time


def run(binary, themes, library):
    with tempfile.TemporaryDirectory(prefix="adm-theme-terminal-") as temporary:
        root = Path(temporary)
        document = root / "doc.c"
        document.write_text("int value = 42;\n")
        config = root / "profile.conf"
        config.write_text(f"[theme]\nfile = {themes}/midnight.theme\n"
                          f"[command t]\nlabel = Insert UTC timestamp\n"
                          f"library = {library}\nfunction = insert_timestamp\n")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        environment = dict(os.environ, ADM_CONFIG=str(config))
        process = subprocess.Popen([str(binary), document.name], cwd=root,
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env=environment)
        os.close(slave)
        terminal = Terminal(24, 80)

        def update(keys=b""):
            if keys:
                os.write(master, keys)
            terminal.feed(read_frame(master))
            return "\n".join(terminal.lines())

        def settle(keys, ready, timeout=5):
            # Frames can take longer than one read on a busy machine (startup
            # loads the configuration, theme and plugin): keep reading until
            # the screen reaches the expected state, then let the asserts run.
            deadline = time.monotonic() + timeout
            screen = update(keys)
            while not ready(screen) and time.monotonic() < deadline:
                screen = update()
            return screen

        stamp = r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZint"
        try:
            settle(b"", lambda text: "int value = 42;" in text)
            row = terminal.lines()[1]
            assert terminal.foregrounds[1][row.index("int")] == 110
            assert terminal.foregrounds[1][row.index("42")] == 173
            assert terminal.backgrounds[1][row.index("value")] == 234
            assert terminal.backgrounds[0][0] == 110
            text = settle(b"\x1bc", lambda text: "USER CENTER" in text)
            assert "USER CENTER" in text and "Insert UTC timestamp" in text
            assert not terminal.cursor_visible
            assert terminal.backgrounds[1][0] == 24
            settle(b"\x1b", lambda text: terminal.cursor_visible)
            assert terminal.cursor_visible
            text = settle(b"\x1bct", lambda text: re.search(stamp, text))
            assert "USER CENTER" not in text
            assert re.search(stamp, text)
            text = settle(b"\x1a", lambda text: "Zint" not in text)
            assert "int value = 42;" in text and "Zint" not in text
            settle(b"\x1bz", lambda text: "Zint" in text)
            saved = stamp + r" value = 42;\n"
            settle(b"\x18\x13", lambda text: re.fullmatch(saved, document.read_text()))
            assert re.fullmatch(saved, document.read_text())
            settle(b"\x1bc", lambda text: "USER CENTER" in text)
            terminal.resize(6, 28)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 6, 28, 0, 0))
            process.send_signal(signal.SIGWINCH)
            text = settle(b"", lambda text: "USER Up/Dn Enter/key Esc" in terminal.lines()[-1])
            assert "USER CENTER" in text and "UTC timestamp" in text
            assert "USER Up/Dn Enter/key Esc" in terminal.lines()[-1]
            settle(b"\x1b", lambda text: terminal.cursor_visible)
            update(b"\x18\x03")
            terminal.feed(wait_exit(process, master))
            assert terminal.background is None and terminal.foreground is None
            assert terminal.cursor_visible
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
    print("Theme and plugin terminal regressions passed.")
