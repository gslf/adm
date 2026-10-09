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

        try:
            assert "int value = 42;" in update()
            row = terminal.lines()[1]
            assert terminal.foregrounds[1][row.index("int")] == 110
            assert terminal.foregrounds[1][row.index("42")] == 173
            assert terminal.backgrounds[1][row.index("value")] == 234
            assert terminal.backgrounds[0][0] == 110
            text = update(b"\x1bc")
            assert "USER CENTER" in text and "Insert UTC timestamp" in text
            assert not terminal.cursor_visible
            assert terminal.backgrounds[1][0] == 24
            update(b"\x1b")
            assert terminal.cursor_visible
            text = update(b"\x1bct")
            assert "USER CENTER" not in text
            assert re.search(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZint", text)
            update(b"\x1a")
            assert "int value = 42;" in update() and "Zint" not in update()
            update(b"\x1bz")
            update(b"\x18\x13")
            assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZint value = 42;\n", document.read_text())
            update(b"\x1bc")
            terminal.resize(6, 28)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 6, 28, 0, 0))
            process.send_signal(signal.SIGWINCH)
            text = update()
            assert "USER CENTER" in text and "UTC timestamp" in text
            assert "USER Up/Dn Enter/key Esc" in terminal.lines()[-1]
            update(b"\x1b")
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
