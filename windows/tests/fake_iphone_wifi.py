"""A fake iPhone that runs Phomic's Wi-Fi protocol against the PC app on this machine.

    python -I fake_iphone_wifi.py            all tests
    python -I fake_iphone_wifi.py ping mute  only those

auto_gain checks automatic gain (on in Phomic unless you turned it off).

Talks to 127.0.0.1:50505 (TCP control + UDP audio) and reads the PC window through UI
Automation (read_phomic.ps1 next to this file) where the result only shows there.
"""
import json
import math
import pathlib
import random
import re
import socket
import struct
import subprocess
import sys
import time
import winreg

HOST, PORT = "127.0.0.1", 50505
HERE = pathlib.Path(__file__).parent
HELLO = {"type": "hello", "version": 1, "name": "TestPhone", "sampleRate": 48000, "channels": 1, "frameSamples": 480}


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=3)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.session = 0
        self.udp_port = 0
        self.seq = 0

    def send(self, msg):
        data = msg if isinstance(msg, (bytes, bytearray)) else (json.dumps(msg) + "\n").encode()
        self.s.sendall(data)

    def line(self, timeout=2.0):
        """Next JSON line, None if the PC closed the connection, "timeout" if nothing came."""
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                return "timeout"
            self.s.settimeout(left)
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                return "timeout"
            except (ConnectionResetError, ConnectionAbortedError):
                return None
            if not chunk:
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return json.loads(raw)

    def closed_within(self, seconds):
        """True if the PC closes the connection within `seconds` (lines on the way are skipped)."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            got = self.line(deadline - time.time())
            if got is None:
                return True
            if got == "timeout":
                return False
        return False

    def welcome(self, hello=None):
        self.send(hello or HELLO)
        reply = self.line()
        if isinstance(reply, dict) and reply.get("type") == "welcome":
            self.session = reply["session"]
            self.udp_port = reply["udpPort"]
        return reply

    def audio(self, frames, session=None, level=0.3):
        for _ in range(frames):
            self.frame([level * math.sin(2 * math.pi * 440 * (self.seq * 480 + i) / 48000) for i in range(480)], session)
            time.sleep(0.01)

    def frame(self, samples, session=None):
        """One 10 ms packet: 480 samples, -1...1."""
        pcm = [max(-32767, min(32767, int(x * 32767))) for x in samples]
        packet = b"PHMC" + struct.pack("<II", session if session is not None else self.session, self.seq) + struct.pack("<480h", *pcm)
        self.udp.sendto(packet, (HOST, self.udp_port))
        self.seq += 1

    def close(self):
        for s in (self.s, self.udp):
            try:
                s.close()
            except OSError:
                pass


def window_text():
    out = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "read_phomic.ps1"), "-All"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    return out.stdout


def click(name):
    """Invokes the first button in the PC window with this name."""
    script = f"""
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$p = Get-Process Phomic | Select-Object -First 1
$c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
$w = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $c)
$n = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, '{name}')
$b = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $n)
if ($b) {{ $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); 'clicked' }} else {{ 'missing' }}
"""
    return subprocess.run(["powershell.exe", "-NoProfile", "-Command", script], capture_output=True, text=True).stdout.strip()


def wait_for_text(fragment, seconds=4.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if fragment in window_text():
            return True
        time.sleep(0.3)
    return False


def phomic_running():
    return "Phomic" in subprocess.run(["tasklist", "/fi", "imagename eq Phomic.exe"], capture_output=True, text=True).stdout


def set_pairing(required):
    """Sets Phomic's "Require a pairing code" and returns what it was (off if never set)."""
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Phomic") as key:
        try:
            old = winreg.QueryValueEx(key, "RequirePairingCode")[0]
        except FileNotFoundError:
            old = 0
        winreg.SetValueEx(key, "RequirePairingCode", 0, winreg.REG_DWORD, 1 if required else 0)
    return old


def pairing_code():
    """The PC's code. Phomic makes one the first time it needs it, so ask after a connection."""
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Phomic") as key:
        return winreg.QueryValueEx(key, "PairingCode")[0]


# ------------------------------------------------------------------ tests

def t_not_hello():
    c = Conn(); c.send({"type": "ping", "id": 1})
    ok = c.closed_within(1.5); c.close()
    return ok, "a first line that isn't hello closes the connection"


def t_garbage():
    c = Conn(); c.send(b"hello world\n")
    ok = c.closed_within(1.5); c.close()
    return ok, "a line that isn't JSON closes the connection"


def t_wrong_field_type():
    c = Conn(); c.send({"type": "hello", "version": "one", "name": 5})
    ok = c.closed_within(1.5); c.close()
    return ok and phomic_running(), "hello with fields of the wrong type: closed, no crash"


def t_wrong_version():
    c = Conn(); c.send(dict(HELLO, version=2))
    reply = c.line(); closed = c.closed_within(1.5); c.close()
    shown = wait_for_text("TestPhone was turned away")
    ok = isinstance(reply, dict) and reply.get("type") == "error" and "different versions" in reply.get("message", "") and closed and shown
    return ok, f"version 2: error + close, PC shows why ({reply})"


def t_wrong_format():
    c = Conn(); c.send(dict(HELLO, sampleRate=44100))
    reply = c.line(); closed = c.closed_within(1.5); c.close()
    ok = isinstance(reply, dict) and reply.get("message") == "Unsupported audio format." and closed
    return ok, f"44.1 kHz: refused ({reply})"


def t_hello_timeout():
    c = Conn(); start = time.time()
    closed = c.closed_within(8); took = time.time() - start; c.close()
    return closed and 4.0 < took < 7.0, f"no hello: closed after {took:.1f} s (expect ~5)"


def t_welcome():
    c = Conn(); reply = c.welcome(); c.send({"type": "bye"}); c.close()
    ok = (isinstance(reply, dict) and reply.get("type") == "welcome" and reply.get("udpPort") == PORT and reply.get("session")
          and len(reply.get("pcId", "")) == 8 and "transport" not in reply)
    return ok, f"welcome over Wi-Fi has udpPort and no transport ({reply})"


def t_ping():
    c = Conn(); c.welcome(); c.send({"type": "ping", "id": 7, "rtt": 12.5})
    reply = c.line(); c.send({"type": "bye"}); c.close()
    return reply == {"type": "pong", "id": 7}, f"ping 7 -> {reply}"


def t_audio_and_mute():
    c = Conn(); c.welcome()
    c.audio(60)
    streaming = wait_for_text("Streaming from TestPhone", 2)
    c.audio(30, session=c.session + 1)  # wrong session: ignored, so audio "stops"
    c.send({"type": "mute", "muted": True}); c.send({"type": "ping", "id": 1, "rtt": 3})
    muted = wait_for_text("TestPhone is muted", 3)
    c.send({"type": "bye"}); c.close()
    return streaming and muted, f"streaming shown: {streaming}, muted shown: {muted}"


def t_bye_from_phone():
    c = Conn(); c.welcome(); c.send({"type": "bye"})
    closed = c.closed_within(2); c.close()
    shown = wait_for_text("The iPhone disconnected.")
    return closed and shown, "phone's bye: PC closes and says so"


def t_lost_contact():
    c = Conn(); c.welcome(); start = time.time()
    got = c.line(9); took = time.time() - start; c.close()
    shown = wait_for_text("Lost contact with the iPhone.")
    return got == {"type": "bye"} and 5.5 < took < 8 and shown, f"silent phone: {got} after {took:.1f} s (expect bye ~6 s)"


def t_newest_wins():
    a = Conn(); a.welcome()
    b = Conn(); rb = b.welcome(dict(HELLO, name="SecondPhone"))
    ra = a.line(); a_closed = a.closed_within(2)
    shown = wait_for_text("SecondPhone")
    b.send({"type": "bye"}); a.close(); b.close()
    return ra == {"type": "bye"} and a_closed and rb.get("type") == "welcome" and shown, f"first got {ra}, second got {rb.get('type')}"


def t_long_line():
    c = Conn(); c.welcome(); c.send(b"x" * 70000)
    got = c.line(3); closed = got is None or c.closed_within(2); c.close()
    # The PC drops it; its "bye" may be lost to the reset that unread data causes on close.
    return got in ({"type": "bye"}, None) and closed and phomic_running(), f"70 KB without newline: {got}"


def t_disconnect_button():
    c = Conn(); c.welcome(); c.audio(20)
    clicked = click("Disconnect")
    got = c.line(3); c.close()
    return clicked == "clicked" and got == {"type": "bye"}, f"PC's Disconnect ({clicked}): {got}"


def t_switch_button():
    c = Conn(); c.welcome(); c.audio(20)
    time.sleep(0.5)
    clicked = click("Switch to Bluetooth")  # while Wi-Fi is in use
    got = c.line(3); c.send({"type": "bye"}); c.close()
    return clicked == "clicked" and got == {"type": "switch", "transport": "bluetooth"}, f"Switch ({clicked}): {got}"


def t_pairing():
    old = set_pairing(True)
    try:
        c = Conn(); c.send(HELLO); r1 = c.line(); c.close()
        code = pairing_code()
        c = Conn(); c.send(dict(HELLO, code="0000" if code != "0000" else "1111")); r2 = c.line(); c.close()
        c = Conn(); r3 = c.welcome(dict(HELLO, code=code)); c.send({"type": "bye"}); c.close()
    finally:
        set_pairing(bool(old))
    ok = (r1.get("reason") == "pairing" and r1.get("message") == "This PC needs a pairing code."
          and r2.get("message") == "Wrong pairing code." and r3.get("type") == "welcome")
    return ok, f"no code: {r1.get('message')} · wrong: {r2.get('message')} · right: {r3.get('type')}"


def auto_gain_on():
    """Whether Phomic uses automatic gain (its setting; 1 unless turned off)."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Phomic") as key:
            return winreg.QueryValueEx(key, "AutoGain")[0] != 0
    except OSError:
        return True


def t_auto_gain():
    """Talking at -35 dBFS (RMS) over a quiet room: automatic gain settles near +12 dB (to -23)."""
    if not auto_gain_on():
        return True, "skipped: automatic gain is off in Phomic"
    c = Conn(); c.welcome()
    rng = random.Random(1)
    tone = 10 ** (-35 / 20) * math.sqrt(2)  # sine amplitude for -35 dB RMS
    noise = 10 ** (-65 / 20) * math.sqrt(3)  # uniform noise at -65 dB RMS
    start = time.time()
    for i in range(900):  # 9 s: 0.7 s "words", 0.15 s gaps
        talking = i % 85 < 70
        c.frame([(tone * math.sin(2 * math.pi * 220 * (c.seq * 480 + n) / 48000) if talking else 0)
                 + noise * rng.uniform(-1, 1) for n in range(480)])
        if i % 100 == 0:
            c.send({"type": "ping", "id": i, "rtt": 3})
        time.sleep(max(0, start + (i + 1) * 0.01 - time.time()))
    text = window_text()
    found = re.search(r"Now ([+-]?\d+) dB", text)
    c.send({"type": "bye"}); c.close()
    gain = int(found.group(1)) if found else None
    return gain is not None and 10 <= gain <= 15, f"automatic gain now {gain} dB (expect ~+12)"


TESTS = [t_not_hello, t_garbage, t_wrong_field_type, t_wrong_version, t_wrong_format, t_hello_timeout, t_welcome,
         t_ping, t_audio_and_mute, t_bye_from_phone, t_lost_contact, t_newest_wins, t_long_line,
         t_disconnect_button, t_switch_button, t_pairing, t_auto_gain]

if __name__ == "__main__":
    wanted = sys.argv[1:]
    failed = 0
    for test in TESTS:
        if wanted and not any(w in test.__name__ for w in wanted):
            continue
        try:
            ok, detail = test()
        except Exception as error:  # noqa: BLE001 - report and go on
            ok, detail = False, f"{type(error).__name__}: {error}"
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {test.__name__[2:]:18} {detail}", flush=True)
        time.sleep(0.5)
    print(f"\n{failed} failed" if failed else "\nall passed")
    print("Phomic still running:", phomic_running())
