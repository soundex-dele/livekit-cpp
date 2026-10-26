"""Opaque UDP forwarding/drop harness. Never parses, constructs or retransmits RTC packets."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import secrets
import selectors
import socket
import threading
import time


class UdpImpairment:
    @staticmethod
    def now():
        # Use an unadjusted clock for test duration measurements in Linux.
        # This does not replace any production/libwebrtc clock.
        return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)

    def __init__(self, upstream):
        self.upstream = upstream
        self.lock = threading.Lock()
        self.until = 0.0
        self.started = 0.0
        self.dropped = {"upstream":0, "downstream":0}
        self.forwarded = {"upstream":0, "downstream":0}
        self.errors = []
        self.shutdown = threading.Event()
        self.token = secrets.token_urlsafe(32)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.setblocking(False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.listener, selectors.EVENT_READ, None)
        self.clients = {}
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                if self.headers.get("Authorization") != "Bearer " + owner.token:
                    self.send_error(403)
                    return
                if self.path == "/outage":
                    with owner.lock:
                        owner.started = owner.now()
                        owner.until = owner.started + 5
                        owner.dropped = {"upstream":0, "downstream":0}
                elif self.path != "/status":
                    self.send_error(404)
                    return
                content = json.dumps(owner.snapshot()).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)

            def log_message(self, *args):
                pass

        self.control = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.control.daemon_threads = True
        self.url = f"http://127.0.0.1:{self.control.server_port}"
        self.network_thread = threading.Thread(target=self.run, daemon=True)
        self.control_thread = threading.Thread(target=self.control.serve_forever, daemon=True)
        self.network_thread.start()
        self.control_thread.start()

    def snapshot(self):
        with self.lock:
            now = self.now()
            return {"clock":"CLOCK_MONOTONIC_RAW", "duration_seconds":5, "active":now < self.until,
                    "elapsed_seconds":now-self.started if self.started else 0,
                    "remaining_seconds":max(0, self.until-now),
                    "dropped":dict(self.dropped), "forwarded":dict(self.forwarded), "errors":list(self.errors)}

    def forward(self, direction, sock, data, destination=None):
        with self.lock:
            if self.now() < self.until:
                self.dropped[direction] += 1
                return
        if destination:
            sock.sendto(data, destination)
        else:
            sock.send(data)
        with self.lock:
            self.forwarded[direction] += 1

    def run(self):
        try:
            while not self.shutdown.is_set():
                for key, _ in self.selector.select(timeout=.1):
                    sock, address = key.fileobj, key.data
                    try:
                        if address is None:
                            data, client = sock.recvfrom(65536)
                            peer = self.clients.get(client)
                            if peer is None:
                                if len(self.clients) >= 256:
                                    raise RuntimeError("UDP test proxy exceeded 256 client sockets")
                                peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                                peer.connect(self.upstream)
                                peer.setblocking(False)
                                self.clients[client] = peer
                                self.selector.register(peer, selectors.EVENT_READ, client)
                            self.forward("upstream", peer, data)
                        else:
                            self.forward("downstream", self.listener, sock.recv(65536), address)
                    except BlockingIOError:
                        pass
        except Exception as error:
            with self.lock:
                self.errors.append(str(error))

    def close(self):
        self.control.shutdown()
        self.control.server_close()
        self.control_thread.join(timeout=2)
        self.shutdown.set()
        self.network_thread.join(timeout=2)
        self.selector.close()
        for sock in self.clients.values():
            sock.close()
        self.listener.close()
        if self.network_thread.is_alive() or self.control_thread.is_alive():
            raise RuntimeError("UDP impairment threads failed to stop")
