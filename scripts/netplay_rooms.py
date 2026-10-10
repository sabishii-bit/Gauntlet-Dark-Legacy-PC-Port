#!/usr/bin/env python3
"""Local development room coordinator, not a publicly deployable matchmaking service.

Rooms allocate four *player seats*, independently of the number of computers.
Bearer credentials authorize membership; the shareable room code is not a peer
identity. Signaling is bounded, best-effort delivery; gameplay never uses HTTP.
The runner embeds this server on 127.0.0.1. A production service still needs TLS,
abuse protection, deployment/monitoring and short-lived STUN/TURN configuration.
No accounts, game assets, saves, durable storage or third-party Python packages.
"""

import collections
import dataclasses
import http.server
import json
import re
import secrets
import threading
import time


class RoomError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


@dataclasses.dataclass
class Member:
    peer: str
    token: str
    seats: list[int]
    seen: float
    ready: bool = False
    signals: collections.deque = dataclasses.field(default_factory=collections.deque)
    credit: float = 128
    credited_at: float = 0


@dataclasses.dataclass
class Room:
    code: str
    compatibility: dict
    host: str
    members: dict[str, Member]
    revision: int = 1
    started: bool = False


class Directory:
    """Atomic state transitions, independently of HTTP connection lifetime.

    A heartbeat expires after 30 seconds; losing the host closes the room. Rejoin
    creates a new identity, never inherits old seats/readiness or queued signals.
    No joining a started session or implicit host migration in the first protocol.
    """
    MAX_ROOMS = 128
    PROTOCOL = 7  # RoomClient::kProtocol; native entry presentation and unanimous movie skipping.
    LEASE_SECONDS = 30
    SIGNAL_BYTES = 8192
    SIGNAL_QUEUE = 64
    POLL_BATCH = 16
    CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

    def __init__(self, clock=time.monotonic):
        self.rooms = {}
        self.clock = clock
        self.lock = threading.Lock()

    @staticmethod
    def fields(body, required):
        if type(body) is not dict or set(body) != set(required):
            raise RoomError(400, "Invalid request fields")

    @staticmethod
    def seats(count):
        if type(count) is not int or not 1 <= count <= 4:
            raise RoomError(400, "Invalid local player count")

    @staticmethod
    def compatibility(value):
        Directory.fields(value, ("protocol", "build", "content"))
        if (type(value["protocol"]) is not int or value["protocol"] != Directory.PROTOCOL
                or not isinstance(value["build"], str)
                or not re.fullmatch(r"[A-Za-z0-9.+-]{1,64}", value["build"])
                or not isinstance(value["content"], str)
                or not re.fullmatch(r"[0-9a-f]{64}", value["content"])):
            raise RoomError(400, "Invalid compatibility identity")
        return dict(value)

    @staticmethod
    def changed(room):
        room.revision += 1
        for member in room.members.values():
            member.ready = False

    def expire(self, now):
        for code, room in list(self.rooms.items()):
            expired = [peer for peer, member in room.members.items()
                       if now - member.seen >= self.LEASE_SECONDS]
            if room.host in expired:
                del self.rooms[code]
                continue
            for peer in expired:
                self.remove(room, peer)

    def remove(self, room, peer):
        del room.members[peer]
        self.changed(room)
        for member in room.members.values():
            member.signals = collections.deque(s for s in member.signals if s["peer"] != peer)

    @staticmethod
    def snapshot(room):
        return {"code": room.code, "host": room.host, "revision": room.revision,
                "started": room.started,
                "members": [{"peer": m.peer, "seats": list(m.seats), "ready": m.ready}
                            for m in room.members.values()]}

    def admit(self, room, count, now):
        occupied = {seat for m in room.members.values() for seat in m.seats}
        free = [seat for seat in range(4) if seat not in occupied]
        if len(free) < count:
            raise RoomError(409, "Room is full")
        member = Member(secrets.token_hex(16), secrets.token_hex(32), free[:count], now,
                        credited_at=now)
        room.members[member.peer] = member
        return member

    def request(self, action, body, token=""):
        # Expiry, admission, readiness, and dequeueing signals must be one
        # transaction. Never hold this lock while reading or writing a socket.
        with self.lock:
            return self._request(action, body, token)

    def _request(self, action, body, token):
        now = self.clock()
        self.expire(now)
        if action in ("create", "join"):
            self.fields(body, ("players", "compatibility") if action == "create"
                        else ("players", "compatibility", "code"))
            self.seats(body["players"])
            compatibility = self.compatibility(body["compatibility"])
            if action == "create":
                if len(self.rooms) >= self.MAX_ROOMS:
                    raise RoomError(503, "Room service is full")
                while True:
                    code = "".join(secrets.choice(self.CODE_ALPHABET) for _ in range(8))
                    if code not in self.rooms:
                        break
                room = Room(code, compatibility, "", {})
                member = self.admit(room, body["players"], now)
                room.host = member.peer
                self.rooms[code] = room
            else:
                room = self.lookup(body["code"])
                if room.started:
                    raise RoomError(409, "Session has already started")
                if room.compatibility != compatibility:
                    raise RoomError(409, "Build, protocol or game data differs")
                member = self.admit(room, body["players"], now)
                self.changed(room)
            return {"peer": member.peer, "token": member.token, "room": self.snapshot(room)}

        fields = {"poll": ("code",), "leave": ("code",),
                  "ready": ("code", "revision", "ready"),
                  "start": ("code", "revision"), "signal": ("code", "peer", "data")}
        if action not in fields:
            raise RoomError(404, "Unknown operation")
        self.fields(body, fields[action])
        room = self.lookup(body["code"])
        if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token):
            raise RoomError(401, "Invalid membership")
        member = next((m for m in room.members.values()
                       if secrets.compare_digest(m.token, token)), None)
        if member is None:
            raise RoomError(401, "Invalid membership")
        member.seen = now
        if action in ("ready", "start"):
            if type(body["revision"]) is not int or body["revision"] != room.revision:
                raise RoomError(409, "Roster changed")
            if room.started:
                raise RoomError(409, "Session has already started")
            if action == "ready":
                if type(body["ready"]) is not bool:
                    raise RoomError(400, "Invalid ready state")
                member.ready = body["ready"]
            else:
                if member.peer != room.host:
                    raise RoomError(403, "Only the host may start")
                if not all(m.ready for m in room.members.values()):
                    raise RoomError(409, "Players are not ready")
                room.started = True
        elif action == "leave":
            if member.peer == room.host:
                del self.rooms[room.code]
            else:
                self.remove(room, member.peer)
            return {}
        elif action == "signal":
            target = body["peer"]
            data = body["data"]
            if (not isinstance(target, str) or target not in room.members or target == member.peer
                    or room.host not in (target, member.peer)):
                raise RoomError(403, "Invalid signaling destination")
            if (not isinstance(data, str) or not 2 <= len(data) <= self.SIGNAL_BYTES * 2
                    or len(data) % 2 or not re.fullmatch(r"[0-9a-f]+", data)):
                raise RoomError(400, "Invalid signal")
            member.credit = min(128, member.credit + (now - member.credited_at) * 64)
            member.credited_at = now
            if member.credit < 1:
                raise RoomError(429, "Signaling rate exceeded")
            member.credit -= 1
            queue = room.members[target].signals
            if len(queue) >= self.SIGNAL_QUEUE:
                queue.popleft()  # GNS retries stale/lost handshake signals.
            queue.append({"peer": member.peer, "data": data})
            return {}
        result = {"room": self.snapshot(room)}
        if action == "poll":
            result["signals"] = [member.signals.popleft()
                                 for _ in range(min(self.POLL_BATCH, len(member.signals)))]
        return result

    def lookup(self, code):
        if not isinstance(code, str) or not re.fullmatch(r"[A-Z2-9]{8}", code):
            raise RoomError(400, "Invalid room code")
        if code not in self.rooms:
            raise RoomError(404, "Room has closed or expired")
        return self.rooms[code]


class RoomServer(http.server.ThreadingHTTPServer):
    """A stalled client cannot monopolize signaling; worker count stays bounded."""
    MAX_CONNECTIONS = 16
    request_queue_size = MAX_CONNECTIONS
    daemon_threads = False  # server_close joins the bounded, timed-out workers.

    def __init__(self, address, handler):
        self.connections = threading.BoundedSemaphore(self.MAX_CONNECTIONS)
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self.connections.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.connections.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.connections.release()


def make_server():
    """Bind only loopback, atomically assigning a free port. No public-listen option."""
    directory = Directory()

    class Handler(http.server.BaseHTTPRequestHandler):
        # cpp-httplib sends Expect: 100-continue for larger ICE messages. The
        # base handler only acknowledges it when HTTP/1.1 is enabled; otherwise
        # every such POST stalls until the client's one-second fallback fires.
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            self.connection.settimeout(2)

        def log_message(self, _format, *args):
            pass  # Never log bearer tokens, bodies or signals.

        def handle(self):
            try:
                super().handle()
            except (OSError, TimeoutError):
                pass  # The runner can kill a failed child during request startup.

        def do_POST(self):
            try:
                if self.headers.get("Transfer-Encoding"):
                    raise RoomError(400, "Chunked requests are not accepted")
                lengths = self.headers.get_all("Content-Length", [])
                if len(lengths) != 1 or not lengths[0].isdigit():
                    raise RoomError(400, "Invalid content length")
                length = int(lengths[0])
                if not 0 < length <= 20000:
                    raise RoomError(413, "Request too large")
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise RoomError(400, "Incomplete body")
                # Consume the already-bounded body before replying. Closing with
                # unread bytes can reset the TCP stream on Windows, hiding the
                # 415 response from a client that sent headers/body separately.
                if self.headers.get("Content-Type") != "application/json":
                    raise RoomError(415, "Expected JSON")
                body = json.loads(raw)
                if not self.path.startswith("/v1/"):
                    raise RoomError(404, "Unknown API")
                auth = self.headers.get("Authorization", "")
                token = auth[7:] if auth.startswith("Bearer ") else ""
                result = directory.request(self.path[4:], body, token)
                status = 200
            except RoomError as error:
                status, result = error.status, {"error": str(error)}
            except (ValueError, RecursionError):
                status, result = 400, {"error": "Invalid JSON"}
            except (OSError, TimeoutError):
                return  # Incomplete/abandoned request; no state change.
            data = json.dumps(result, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(data)
            except OSError:
                pass

    return RoomServer(("127.0.0.1", 0), Handler)
