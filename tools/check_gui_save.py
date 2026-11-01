#!/usr/bin/env python3
"""Exercise the actual exit/save dialogs using X11 keyboard events.

Requires a running X11/XWayland display, libX11 and libXtst (XTest).
Uses only Python's standard library. Each GUI runs in its own directory
under build/, without a nested build/ folder or a --svg automation argument.

    pixi run gui
    python3 tools/check_gui_save.py
"""
import ctypes as C
import ctypes.util
from pathlib import Path
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET


def require(condition, message):
    """Report the first failed interaction with enough context to reproduce it."""
    if not condition:
        raise RuntimeError(message)


class Keyboard:
    """Own one X connection and focus the launched GUI by its owner PID."""

    def __init__(self):
        """Bind the small X11/XTest surface needed by this regression check."""
        x11 = ctypes.util.find_library("X11")
        xtst = ctypes.util.find_library("Xtst")
        require(x11 and xtst, "X11 and XTest libraries are required")
        self.x = C.CDLL(x11)
        self.test = C.CDLL(xtst)
        signatures = {
            "XOpenDisplay": ([C.c_char_p], C.c_void_p),
            "XDefaultRootWindow": ([C.c_void_p], C.c_ulong),
            "XInternAtom": ([C.c_void_p, C.c_char_p, C.c_int], C.c_ulong),
            "XQueryTree": ([C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong),
                            C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ulong)),
                            C.POINTER(C.c_uint)], C.c_int),
            "XGetWindowProperty": ([C.c_void_p, C.c_ulong, C.c_ulong, C.c_long,
                                    C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong),
                                    C.POINTER(C.c_int), C.POINTER(C.c_ulong),
                                    C.POINTER(C.c_ulong), C.POINTER(C.c_void_p)], C.c_int),
            "XSetInputFocus": ([C.c_void_p, C.c_ulong, C.c_int, C.c_ulong], C.c_int),
            "XRaiseWindow": ([C.c_void_p, C.c_ulong], C.c_int),
            "XStringToKeysym": ([C.c_char_p], C.c_ulong),
            "XKeysymToKeycode": ([C.c_void_p, C.c_ulong], C.c_ubyte),
            "XFlush": ([C.c_void_p], C.c_int),
            "XFree": ([C.c_void_p], C.c_int),
            "XCloseDisplay": ([C.c_void_p], C.c_int),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.x, name)
            function.argtypes, function.restype = arguments, result
        self.test.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.test.XTestFakeKeyEvent.restype = C.c_int
        self.display = self.x.XOpenDisplay(None)
        require(self.display, "cannot open the X11 display")
        self.root = self.x.XDefaultRootWindow(self.display)
        self.pid_atom = self.x.XInternAtom(self.display, b"_NET_WM_PID", 0)

    def process_id(self, window):
        """Read the owner PID so another running viewer is never targeted."""
        kind, bits, count, remaining, data = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong(), C.c_void_p()
        status = self.x.XGetWindowProperty(self.display, window, self.pid_atom,
                                          0, 1, 0, 0, C.byref(kind), C.byref(bits),
                                          C.byref(count), C.byref(remaining), C.byref(data))
        try:
            if status == 0 and bits.value == 32 and count.value == 1:
                return C.cast(data, C.POINTER(C.c_ulong))[0]
            return None
        finally:
            if data:
                self.x.XFree(data)

    def children(self, window):
        """Read a window's children, including clients reparented by a window manager."""
        root, parent, items, count = C.c_ulong(), C.c_ulong(), C.POINTER(C.c_ulong)(), C.c_uint()
        if not self.x.XQueryTree(self.display, window, C.byref(root), C.byref(parent),
                                C.byref(items), C.byref(count)):
            return []
        try:
            return list(items[:count.value])
        finally:
            if items:
                self.x.XFree(items)

    def focus(self, process):
        """Wait for this GUI's client window, then give it keyboard focus."""
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            require(process.poll() is None, "GUI exited before its window appeared")
            windows = self.children(self.root)
            for _ in range(3):
                next_level = []
                for window in windows:
                    if self.process_id(window) == process.pid:
                        time.sleep(1.0)
                        self.x.XRaiseWindow(self.display, window)
                        self.x.XSetInputFocus(self.display, window, 2, 0)
                        self.x.XFlush(self.display)
                        time.sleep(0.3)
                        return
                    next_level.extend(self.children(window))
                windows = next_level
            time.sleep(0.1)
        raise RuntimeError("GUI client window was not found")

    def key(self, name, modifier=None):
        """Send one complete key chord, with enough frames for ImGui to consume it."""
        code = self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(name.encode("ascii")))
        require(code, f"key unavailable: {name}")
        mod = (self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(modifier.encode("ascii")))
               if modifier else 0)
        if mod:
            self.test.XTestFakeKeyEvent(self.display, mod, 1, 0)
            self.x.XFlush(self.display)
            time.sleep(0.06)
        self.test.XTestFakeKeyEvent(self.display, code, 1, 0)
        self.x.XFlush(self.display)
        time.sleep(0.08)
        self.test.XTestFakeKeyEvent(self.display, code, 0, 0)
        if mod:
            self.test.XTestFakeKeyEvent(self.display, mod, 0, 0)
        self.x.XFlush(self.display)
        time.sleep(0.2)

    def text(self, value):
        """Type the simple unshifted ASCII paths used by these test fixtures."""
        for character in value:
            code = self.x.XKeysymToKeycode(self.display, ord(character))
            require(code, f"character unavailable: {character}")
            self.test.XTestFakeKeyEvent(self.display, code, 1, 0)
            self.x.XFlush(self.display)
            time.sleep(0.04)
            self.test.XTestFakeKeyEvent(self.display, code, 0, 0)
            self.x.XFlush(self.display)
            time.sleep(0.04)
        time.sleep(0.2)

    def close(self):
        """Release this test's X connection after its client windows have closed."""
        self.x.XCloseDisplay(self.display)


def exercise(keyboard, binary, job, multiple=False, cancel=False, retry=False):
    """Drive Quit -> Save and quit -> filename -> Save, including failure recovery."""
    with tempfile.TemporaryDirectory(prefix="gui-save-", dir="build") as name:
        directory = Path(name).resolve()
        command = [str(binary), str(job)]
        if multiple:
            command += ["--sheets", "2", "--search", "128"]
        process = subprocess.Popen(command, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            keyboard.focus(process)
            keyboard.key("q", "Control_L")
            keyboard.key("Tab")  # initialise navigation on the first Quit action
            keyboard.key("Return")  # first Quit action: Save and quit
            require(process.poll() is None and not list(directory.glob("*.svg")),
                    "Save and quit skipped the filename dialog")
            if cancel:
                keyboard.key("Escape")
                require(process.poll() is None, "cancelling Save closed the GUI")
                keyboard.key("s", "Control_L")  # a normal save must not retain pending quit
            if retry:
                keyboard.key("Return")  # activate filename input
                keyboard.key("a", "Control_L")
                keyboard.text("missing/result.svg")
            keyboard.key("Tab")  # filename -> Save
            keyboard.key("Return")
            if retry:
                require(process.poll() is None and not list(directory.glob("*.svg")),
                        "a failed write completed the pending quit")
                keyboard.key("Tab", "Shift_L")  # tab focus activates the filename field
                keyboard.key("a", "Control_L")
                keyboard.text("recovered.svg")
                keyboard.key("Tab")
                keyboard.key("Return")
            if cancel:
                require(process.poll() is None, "normal Save retained a cancelled quit action")
                keyboard.key("q", "Control_L")  # saved result closes without a dialog
            stdout, stderr = process.communicate(timeout=10)
            require(process.returncode == 0, f"GUI failed: {stderr.decode()}")
            stem = "recovered" if retry else "nest"
            paths = ([directory / f"{stem}-001.svg", directory / f"{stem}-002.svg"]
                     if multiple else [directory / f"{stem}.svg"])
            require(all(path.is_file() for path in paths),
                    f"SVGs were not saved in the working directory: {list(directory.iterdir())}")
            require(not (directory / "build").exists(), "export inserted an extra build directory")
            counts = [len(ET.parse(path).getroot().findall("{http://www.w3.org/2000/svg}g"))
                      for path in paths]
            require(counts == ([1, 2] if multiple else [1]), f"wrong exported plates: {counts}")
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)


def main():
    """Check the user's actual exit/save workflow, independently of CLI export hooks."""
    binary = Path("build/bp_gui").resolve()
    job = Path("jobs/multisheet.job").resolve()
    require(binary.is_file(), "build the GUI with pixi run gui first")
    keyboard = Keyboard()
    try:
        print("checking single-plate Save and quit", flush=True)
        exercise(keyboard, binary, job)
        print("checking multi-plate save after Cancel", flush=True)
        exercise(keyboard, binary, job, multiple=True, cancel=True)
        print("checking recovery from a failed write", flush=True)
        exercise(keyboard, binary, job, multiple=True, retry=True)
    finally:
        keyboard.close()
    print("GUI exit/save checks passed: filename dialog, cancel, retry, single and multi export")


if __name__ == "__main__":
    main()
