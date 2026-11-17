"""Tests Phomic's USB path without an iPhone: a fake usbmux (Apple's USB service) with fake iPhones.

Start Phomic with PHOMIC_USBMUX_PORT=27115 (and USB on), so it talks to this fake instead of
Apple's service, then:
    python -I fake_usbmux.py            all tests
    python -I fake_usbmux.py detach     only those
"""
import json
import math
import plistlib
import queue
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent))
import fake_iphone_wifi as wifi  # noqa: E402  (window helpers and the Wi-Fi fake phone)

MUX_PORT = 27115
PHONE_PORT = 50505
HELLO = {"type": "hello", "version": 1, "name": "FakeUSB", "sampleRate": 48000, "channels": 1, "frameSamples": 480}


def frame(msg, tag):
    body = plistlib.dumps(msg)
    return struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body


def read_frame(sock):
    head = b""
    while len(head) < 16:
        chunk = sock.recv(16 - len(head))
        if not chunk:
            return None, None
        head += chunk
    length, _, _, tag = struct.unpack("<IIII", head)
    body = b""
    while len(body) < length - 16:
        chunk = sock.recv(length - 16 - len(body))
        if not chunk:
            return None, None
        body += chunk
    return plistlib.loads(body), tag


class FakeMux:
    def __init__(self):
        self.lock = threading.Lock()
        self.devices = {}          # id -> listening (True/False), or "broken": Connect gets no answer
        self.listeners = []        # Listen connections (Phomic's watcher)
        self.tunnels = queue.Queue()
        self.attempts = 0
        self.server = None
        self.all_socks = []

    def start(self):
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", MUX_PORT))
        self.server.listen(16)
        threading.Thread(target=self._accept, args=(self.server,), daemon=True).start()

    def stop(self):
        """Apple's service going away: everything closes."""
        with self.lock:
            socks = [self.server] + self.listeners + self.all_socks
            self.listeners, self.all_socks = [], []
        for s in socks:
            try:
                s.close()
            except OSError:
                pass

    def _accept(self, server):
        while True:
            try:
                client, _ = server.accept()
            except OSError:
                return
            with self.lock:
                self.all_socks.append(client)
            threading.Thread(target=self._serve, args=(client,), daemon=True).start()

    def _serve(self, client):
        msg, tag = read_frame(client)
        if msg is None:
            return
        kind = msg.get("MessageType")
        if kind == "Listen":
            client.sendall(frame({"MessageType": "Result", "Number": 0}, tag))
            with self.lock:
                self.listeners.append(client)
                devices = list(self.devices)
            for device in devices:
                self._send(client, self._attached(device))
            return  # stays open; events go out from attach/detach
        if kind == "Connect":
            device = msg.get("DeviceID")
            port = socket.ntohs(msg.get("PortNumber", 0))
            with self.lock:
                self.attempts += 1
                state = self.devices.get(device)
                ok = state is True and port == PHONE_PORT
            if state == "broken":
                client.close()  # what Apple's service did with an iPhone it couldn't reach
                return
            if ok:
                client.sendall(frame({"MessageType": "Result", "Number": 0}, tag))
                self.tunnels.put(client)  # from now on the test is "the iPhone"
            else:
                client.sendall(frame({"MessageType": "Result", "Number": 3}, tag))
                try:
                    client.recv(1)  # let Phomic close first, like Apple's service
                except OSError:
                    pass
                client.close()

    @staticmethod
    def _attached(device):
        return {"MessageType": "Attached", "DeviceID": device,
                "Properties": {"ConnectionType": "USB", "DeviceID": device, "ProductID": 0x12A8, "SerialNumber": "FAKE"}}

    def _send(self, sock, msg):
        try:
            sock.sendall(frame(msg, 0))
        except OSError:
            pass

    def attach(self, device, listening):
        with self.lock:
            new = device not in self.devices
            self.devices[device] = listening
            listeners = list(self.listeners)
        if new:
            for s in listeners:
                self._send(s, self._attached(device))

    def set_listening(self, device, listening):
        with self.lock:
            self.devices[device] = listening

    def detach(self, device):
        with self.lock:
            self.devices.pop(device, None)
            listeners = list(self.listeners)
        for s in listeners:
            self._send(s, {"MessageType": "Detached", "DeviceID": device})

    def reset(self):
        for device in list(self.devices):
            self.detach(device)
        while not self.tunnels.empty():
            try:
                self.tunnels.get_nowait().close()
            except OSError:
                pass

    def tunnel(self, timeout=3.0):
        try:
            return self.tunnels.get(timeout=timeout)
        except queue.Empty:
            return None


class Phone:
    """The iPhone end of a USB tunnel: the Wi-Fi protocol, with audio on the same connection."""

    def __init__(self, sock):
        self.s = sock
        self.buf = b""
        self.session = 0
        self.seq = 0

    send = wifi.Conn.send
    line = wifi.Conn.line
    closed_within = wifi.Conn.closed_within

    def welcome(self, hello=None):
        self.send(hello or HELLO)
        reply = self.line()
        if isinstance(reply, dict) and reply.get("type") == "welcome":
            self.session = reply["session"]
        return reply

    def audio(self, frames, level=0.3):
        for _ in range(frames):
            samples = [int(level * 32767 * math.sin(2 * math.pi * 440 * (self.seq * 480 + i) / 48000)) for i in range(480)]
            self.s.sendall(b"PHMC" + struct.pack("<II", self.session, self.seq) + struct.pack("<480h", *samples))
            self.seq += 1
            time.sleep(0.01)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


mux = FakeMux()


def usb_row():
    text = wifi.window_text().splitlines()
    for i, line in enumerate(text):
        if line.strip() == "Text: USB" and i + 1 < len(text):
            return text[i + 1].replace("Text: ", "")
    return ""


def wait_row(fragment, seconds=5.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        row = usb_row()
        if fragment in row:
            return True, row
        time.sleep(0.3)
    return False, usb_row()


def session(name="FakeUSB"):
    """Plugs in a listening fake iPhone and returns it once welcomed."""
    mux.attach(1, True)
    tunnel = mux.tunnel()
    if not tunnel:
        return None, None
    phone = Phone(tunnel)
    return phone, phone.welcome(dict(HELLO, name=name))


# ------------------------------------------------------------------ tests

def t_refused_retries():
    def phomic_time_waits():  # Phomic's own ends of connections to the service
        out = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                              f"@(Get-NetTCPConnection -State TimeWait -RemotePort {MUX_PORT} -ErrorAction SilentlyContinue).Count"],
                             capture_output=True, text=True).stdout.strip()
        return int(out or 0)
    mux.attach(1, False)
    time.sleep(0.5)
    before, tw_before = mux.attempts, phomic_time_waits()
    time.sleep(3)
    tries = mux.attempts - before
    tw = phomic_time_waits() - tw_before
    shown, row = wait_row("iPhone plugged in")
    return 7 <= tries <= 13 and shown and tw <= 0, f"{tries} tries in 3 s (expect ~10), row: {row!r}, new TIME_WAIT: {tw}"


def t_connects_fast():
    mux.attach(1, False)
    time.sleep(0.6)
    start = time.time()
    mux.set_listening(1, True)
    tunnel = mux.tunnel()
    took = time.time() - start
    if tunnel:
        tunnel.close()
    return tunnel is not None and took < 0.7, f"picked up {took:.2f} s after the phone started listening (expect < 0.4)"


def t_welcome_usb():
    phone, reply = session()
    ok = (isinstance(reply, dict) and reply.get("type") == "welcome" and reply.get("transport") == "usb"
          and "udpPort" not in reply and reply.get("session"))
    if phone:
        phone.send({"type": "bye"}); phone.close()
    return ok, f"{reply}"


def t_audio_ping_mute():
    phone, _ = session()
    phone.audio(60)
    streaming = wifi.wait_for_text("Streaming from FakeUSB", 2) and "USB cable" in wifi.window_text()
    no_disconnect = "Button: Disconnect" not in wifi.window_text()
    phone.send({"type": "ping", "id": 9, "rtt": 1.5})
    pong = phone.line()
    phone.send({"type": "mute", "muted": True})
    muted = wifi.wait_for_text("FakeUSB is muted", 3)
    phone.send({"type": "bye"}); phone.close()
    return streaming and no_disconnect and pong == {"type": "pong", "id": 9} and muted, \
        f"streaming: {streaming}, no Disconnect: {no_disconnect}, pong: {pong}, muted: {muted}"


def t_bad_packet():
    phone, _ = session()
    phone.s.sendall(b"PXXX" + bytes(968))
    got = phone.line(2); closed = phone.closed_within(2); phone.close()
    return got == {"type": "bye"} and closed and wifi.phomic_running(), f"bad magic: {got}"


def t_unplugged():
    phone, _ = session()
    phone.audio(20)
    mux.detach(1)
    closed = phone.closed_within(2)
    shown = wifi.wait_for_text("The iPhone was unplugged.", 3)
    phone.close()
    return closed and shown, f"closed: {closed}, PC says unplugged: {shown}"


def t_phone_bye_retry():
    phone, _ = session()
    phone.send({"type": "bye"})
    phone.closed_within(2); phone.close()
    start = time.time()
    tunnel = mux.tunnel(3)  # still listening: the PC comes straight back
    took = time.time() - start
    if tunnel:
        tunnel.close()
    return tunnel is not None and took < 1.0, f"back after {took:.2f} s (expect < 0.7)"


def t_hello_timeout():
    mux.attach(1, True)
    tunnel = mux.tunnel()
    phone = Phone(tunnel)
    mux.set_listening(1, False)
    start = time.time(); closed = phone.closed_within(8); took = time.time() - start; phone.close()
    return closed and 4 < took < 7, f"no hello: closed after {took:.1f} s"


def t_wrong_version():
    mux.attach(1, True)
    phone = Phone(mux.tunnel())
    mux.set_listening(1, False)
    phone.send(dict(HELLO, version=2))
    got = phone.line(); closed = phone.closed_within(2); phone.close()
    shown = wifi.wait_for_text("FakeUSB was turned away", 3)
    return isinstance(got, dict) and got.get("type") == "error" and closed and shown, f"{got}, PC shows why: {shown}"


def t_switch_here():
    phone, _ = session()
    phone.audio(20)
    time.sleep(0.6)
    clicked = wifi.click("Switch here")  # the first one: Wi-Fi
    got = phone.line(3)
    phone.send({"type": "bye"}); phone.close()
    return got == {"type": "switch", "transport": "wifi"}, f"Switch here ({clicked}): {got}"


def t_wifi_takes_over():
    phone, _ = session()
    phone.audio(10)
    mux.set_listening(1, False)  # as the real iPhone stops waiting after "switched"
    w = wifi.Conn(); rw = w.welcome(dict(wifi.HELLO, name="WiFiPhone"))
    got = phone.line(3); phone.close()
    w.send({"type": "bye"}); w.close()
    return got == {"type": "bye", "reason": "switched"} and rw.get("type") == "welcome", f"USB phone got {got}"


def t_usb_takes_over():
    w = wifi.Conn(); w.welcome(dict(wifi.HELLO, name="WiFiPhone")); w.audio(10)
    phone, reply = session()
    got = w.line(3); w.close()
    if phone:
        phone.send({"type": "bye"}); phone.close()
    return got == {"type": "bye"} and isinstance(reply, dict) and reply.get("type") == "welcome", f"Wi-Fi phone got {got}"


def t_service_gone():
    phone, _ = session()
    phone.audio(10)
    mux.stop()  # closes every connection, the tunnel included, like the real service dying
    shown = wifi.wait_for_text("Apple's USB service stopped.", 3)
    waiting, row = wait_row("Waiting for Apple's USB service", 4)
    mux.__init__(); mux.start(); mux.attach(1, True)
    tunnel = mux.tunnel(6)  # the PC finds the service again and connects
    back = None
    if tunnel:
        p = Phone(tunnel); back = p.welcome(); p.send({"type": "bye"}); p.close()
    ok = shown and waiting and isinstance(back, dict) and back.get("type") == "welcome"
    return ok, f"reason shown: {shown}, row: {row!r}, back: {back and back.get('type')}"


def t_unreachable():
    mux.attach(1, "broken")
    shown, row = wait_row("can't reach the iPhone", 10)
    mux.set_listening(1, True)  # it comes back (plugged in again)
    tunnel = mux.tunnel(3)
    if tunnel:
        p = Phone(tunnel); p.welcome(); p.send({"type": "bye"}); p.close()
    cleared, _ = wait_row("plugged in", 4)
    return shown and tunnel is not None, f"row: {row!r}, connects once it answers: {tunnel is not None}"


TESTS = [t_refused_retries, t_connects_fast, t_welcome_usb, t_audio_ping_mute, t_bad_packet, t_unplugged,
         t_phone_bye_retry, t_hello_timeout, t_wrong_version, t_switch_here, t_wifi_takes_over, t_usb_takes_over,
         t_service_gone, t_unreachable]

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    wanted = sys.argv[1:]
    mux.start()
    time.sleep(4)  # Phomic looks for the service every 3 s
    failed = 0
    for test in TESTS:
        if wanted and not any(w in test.__name__ for w in wanted):
            continue
        mux.reset()
        time.sleep(1.2)
        try:
            ok, detail = test()
        except Exception as error:  # noqa: BLE001
            ok, detail = False, f"{type(error).__name__}: {error}"
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {test.__name__[2:]:18} {detail}", flush=True)
    mux.reset()
    print(f"\n{failed} failed" if failed else "\nall passed")
    print("Phomic still running:", wifi.phomic_running())
