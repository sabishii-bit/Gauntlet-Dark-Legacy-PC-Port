"""Room admission and signaling are independent of sockets, rendering and assets."""

import copy
import concurrent.futures
import contextlib
import http.client
import json
import pathlib
import secrets
import socket
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
from netplay_rooms import Directory, RoomError, make_server


class RoomTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.directory = Directory(clock=lambda: self.now)
        self.compatibility = {"protocol": Directory.PROTOCOL, "build": "0.1.6-alpha.3", "content": "a" * 64}
        self.host = self.enter(1)

    def enter(self, players, code=None, compatibility=None):
        body = {"players": players, "compatibility": compatibility or self.compatibility}
        if code:
            body["code"] = code
        return self.directory.request("join" if code else "create", body)

    def call(self, who, action, **fields):
        return self.directory.request(action, {"code": who["room"]["code"], **fields}, who["token"])

    def reject(self, status, operation):
        with self.assertRaises(RoomError) as caught:
            operation()
        self.assertEqual(caught.exception.status, status)

    def guest(self, players=1):
        return self.enter(players, self.host["room"]["code"])

    def test_random_room_and_peer_credentials_are_distinct_and_private(self):
        other = self.enter(1)
        self.assertNotEqual(other["room"]["code"], self.host["room"]["code"])
        self.assertNotEqual(other["token"], self.host["token"])
        self.assertEqual(len(self.host["token"]), 64)
        self.assertEqual(len(self.host["peer"]), 32)
        serialized = json.dumps(self.call(self.host, "poll"))
        self.assertNotIn(self.host["token"], serialized)
        self.assertNotIn('"token"', serialized)

    def test_four_seats_not_four_computers(self):
        guest = self.guest(3)
        self.assertEqual([m["seats"] for m in guest["room"]["members"]], [[0], [1, 2, 3]])
        self.reject(409, self.guest)
        self.call(guest, "leave")
        replacement = self.guest(3)
        self.assertNotEqual(guest["peer"], replacement["peer"])
        self.reject(401, lambda: self.call(guest, "poll"))
        self.assertEqual(replacement["room"]["members"][1]["seats"], [1, 2, 3])

    def test_two_local_players_on_each_computer_and_four_on_host(self):
        host = self.enter(2)
        guest = self.enter(2, host["room"]["code"])
        self.assertEqual([m["seats"] for m in guest["room"]["members"]], [[0, 1], [2, 3]])
        full = self.enter(4)
        self.reject(409, lambda: self.enter(1, full["room"]["code"]))

    def test_admission_rejects_wrong_build_protocol_and_assets_without_seat_leak(self):
        for key, value in (("build", "0.1.7-alpha.3"), ("content", "b" * 64),
                           ("protocol", 1), ("protocol", 2), ("protocol", Directory.PROTOCOL + 1)):
            incompatible = {**self.compatibility, key: value}
            self.reject(400 if key == "protocol" else 409,
                        lambda: self.enter(1, self.host["room"]["code"], incompatible))
        self.assertEqual(len(self.call(self.host, "poll")["room"]["members"]), 1)

    def test_malformed_inputs_and_bool_are_not_player_counts(self):
        for count in (True, False, 0, 5, -1, 1.5, "1"):
            self.reject(400, lambda: self.enter(count))
        for code in ([], None, "", "../12345", "abcd1234"):
            self.reject(400, lambda: self.directory.request("poll", {"code": code}))
        self.reject(400, lambda: self.directory.request("create", {}))
        self.reject(404, lambda: self.directory.request("admin", {}))

    def test_join_invalidates_ready_and_rejects_stale_revision(self):
        self.call(self.host, "ready", revision=1, ready=True)
        guest = self.guest()
        self.assertFalse(guest["room"]["members"][0]["ready"])
        self.reject(409, lambda: self.call(self.host, "start", revision=1))
        self.reject(409, lambda: self.call(self.host, "ready", revision=1, ready=True))
        self.reject(400, lambda: self.call(self.host, "ready", revision=2, ready=1))

    def test_host_only_start_requires_current_roster_ready_and_blocks_late_join(self):
        guest = self.guest()
        revision = guest["room"]["revision"]
        self.reject(403, lambda: self.call(guest, "start", revision=revision))
        self.reject(409, lambda: self.call(self.host, "start", revision=revision))
        for who in (self.host, guest):
            self.call(who, "ready", revision=revision, ready=True)
        self.assertTrue(self.call(self.host, "start", revision=revision)["room"]["started"])
        self.reject(409, self.guest)
        self.reject(409, lambda: self.call(self.host, "start", revision=revision))

    def test_room_code_cannot_replace_bearer_credential_or_cross_rooms(self):
        stolen = copy.deepcopy(self.host)
        for token in ("", self.host["room"]["code"], "0" * 64, self.enter(1)["token"]):
            stolen["token"] = token
            self.reject(401, lambda: self.call(stolen, "poll"))

    def test_signal_sender_is_stamped_and_cross_guest_or_cross_room_routing_refused(self):
        guest = self.guest()
        second = self.guest()
        for target in (second["peer"], self.enter(1)["peer"], guest["peer"]):
            self.reject(403, lambda: self.call(guest, "signal", peer=target, data="abcd"))
        self.reject(400, lambda: self.call(guest, "signal", peer=self.host["peer"], data="aa", sender="x"))
        self.call(guest, "signal", peer=self.host["peer"], data="0001abcd")
        self.assertEqual(self.call(self.host, "poll")["signals"],
                         [{"peer": guest["peer"], "data": "0001abcd"}])
        self.assertEqual(self.call(self.host, "poll")["signals"], [])

    def test_signal_validation_bounded_queue_poll_batch_and_rate_limit(self):
        guest = self.guest()
        for data in ("", "g0", "0", "00" * 8193, [], "AA"):
            self.reject(400, lambda: self.call(guest, "signal", peer=self.host["peer"], data=data))
        for i in range(128):
            self.call(guest, "signal", peer=self.host["peer"], data=f"{i:02x}")
        self.reject(429, lambda: self.call(guest, "signal", peer=self.host["peer"], data="ff"))
        batch = self.call(self.host, "poll")["signals"]
        self.assertEqual(len(batch), 16)
        self.assertEqual(batch[0]["data"], "40")
        self.now += 1
        self.call(guest, "signal", peer=self.host["peer"], data="ff")

    def test_expiry_is_not_extended_by_strangers_and_host_departure_closes_room(self):
        guest = self.guest()
        self.now = 29
        self.call(guest, "poll")
        self.now = 30
        self.reject(404, lambda: self.call(guest, "poll"))
        self.assertEqual(self.directory.rooms, {})

    def test_guest_expiry_reclaims_seats_and_erases_queued_signals(self):
        guest = self.guest()
        self.call(guest, "signal", peer=self.host["peer"], data="aa")
        self.now = 29
        self.call(self.host, "ready", revision=2, ready=True)
        self.now = 30
        room = self.call(self.host, "poll")
        self.assertEqual(room["signals"], [])
        self.assertEqual(len(room["room"]["members"]), 1)
        self.assertFalse(room["room"]["members"][0]["ready"])
        self.reject(401, lambda: self.call(guest, "poll"))
        self.guest(3)

    def test_host_leave_and_room_capacity(self):
        guest = self.guest()
        self.call(self.host, "leave")
        self.reject(404, lambda: self.call(guest, "poll"))
        for _ in range(self.directory.MAX_ROOMS):
            self.enter(1)
        self.reject(503, lambda: self.enter(1))
        self.now = 30
        self.enter(1)
        self.assertEqual(len(self.directory.rooms), 1)

    def test_concurrent_joins_cannot_assign_the_last_seat_twice(self):
        host = self.enter(3)
        ready = threading.Barrier(8)
        token_hex = secrets.token_hex

        def slow_token(count):
            # Credential generation can release the GIL. Force that interleave
            # after admit() has selected its free seat, before it stores a member.
            time.sleep(0.01)
            return token_hex(count)

        def join(_index):
            ready.wait(timeout=3)
            try:
                self.enter(1, host["room"]["code"])
                return 200
            except RoomError as error:
                return error.status

        with mock.patch("netplay_rooms.secrets.token_hex", side_effect=slow_token):
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                statuses = list(pool.map(join, range(8)))
        self.assertEqual(sorted(statuses), [200] + [409] * 7)
        self.assertEqual([m["seats"] for m in self.call(host, "poll")["room"]["members"]],
                         [[0, 1, 2], [3]])


class RoomHttpTests(unittest.TestCase):
    def setUp(self):
        self.server = make_server()
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.addCleanup(self.close)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(3)
        self.assertFalse(self.worker.is_alive())

    def request(self, body, headers=None, path="/v1/create", timeout=3):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=timeout)
        self.addCleanup(connection.close)
        connection.request("POST", path, body, headers or {"Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())

    def test_loopback_http_create_and_invalid_requests(self):
        self.assertEqual(self.server.server_address[0], "127.0.0.1")
        status, result = self.request(json.dumps({"players": 1, "compatibility": {
            "protocol": Directory.PROTOCOL, "build": "test-1", "content": "a" * 64}}))
        self.assertEqual(status, 200)
        self.assertEqual(len(result["room"]["code"]), 8)
        for raw, expected in (("{", 400), ("[]", 400), ("0" * 20001, 413)):
            self.assertEqual(self.request(raw)[0], expected)
        self.assertEqual(self.request("{}", {"Content-Type": "text/plain"})[0], 415)
        self.assertEqual(self.request("{}", path="/v0/create")[0], 404)

    def test_non_json_body_is_consumed_before_connection_close(self):
        for _ in range(20):
            self.assertEqual(self.request("x" * 20000, {"Content-Type": "text/plain"})[0], 415)

    def test_idle_and_incomplete_connections_do_not_stall_other_players(self):
        accepted = threading.Event()
        setup = self.server.RequestHandlerClass.setup

        def observe_setup(handler):
            setup(handler)
            accepted.set()

        self.server.RequestHandlerClass.setup = observe_setup
        for request in (b"", b"POST /v1/create HTTP/1.0\r\nContent-Length: 100\r\n"
                            b"Content-Type: application/json\r\n\r\n{"):
            accepted.clear()
            with socket.create_connection(self.server.server_address, timeout=1) as stalled:
                # Wait until the server owns this connection, not merely until
                # connect() places it in the listening socket's backlog.
                if request:
                    stalled.sendall(request)
                self.assertTrue(accepted.wait(1), "server did not accept stalled client")
                status, _ = self.request(json.dumps({"players": 1, "compatibility": {
                    "protocol": Directory.PROTOCOL, "build": "test-1", "content": "a" * 64}}), timeout=0.75)
                self.assertEqual(status, 200)

    def test_expect_continue_unblocks_body_without_client_fallback_timeout(self):
        # cpp-httplib automatically waits for 100 Continue above 1024 bytes.
        # Hex-encoded ICE handshakes exceed that threshold. An HTTP/1.0
        # responder ignores Expect and adds a full second to EVERY signal.
        body = json.dumps({"players": 1, "compatibility": {
            "protocol": Directory.PROTOCOL, "build": "test-1", "content": "a" * 64}}).encode() + b" " * 1024
        with socket.create_connection(self.server.server_address, timeout=0.75) as connection:
            connection.sendall((f"POST /v1/create HTTP/1.1\r\nHost: localhost\r\n"
                                f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n"
                                "Expect: 100-continue\r\nConnection: close\r\n\r\n").encode())
            with connection.makefile("rb") as response:
                self.assertEqual(response.readline(), b"HTTP/1.1 100 Continue\r\n")
                self.assertEqual(response.readline(), b"\r\n")
            connection.sendall(body)
            response = http.client.HTTPResponse(connection)
            response.begin()
            self.assertEqual(response.status, 200)
            self.assertEqual(len(json.loads(response.read())["room"]["code"]), 8)
            response.close()

    def test_connection_capacity_is_bounded_and_reclaimed(self):
        active = 0
        changed = threading.Condition()
        setup = self.server.RequestHandlerClass.setup
        finish = self.server.RequestHandlerClass.finish

        def track_setup(handler):
            nonlocal active
            setup(handler)
            with changed:
                active += 1
                changed.notify_all()

        def track_finish(handler):
            nonlocal active
            try:
                finish(handler)
            finally:
                with changed:
                    active -= 1
                    changed.notify_all()

        self.server.RequestHandlerClass.setup = track_setup
        self.server.RequestHandlerClass.finish = track_finish
        with contextlib.ExitStack() as stack:
            for _ in range(self.server.MAX_CONNECTIONS):
                stack.enter_context(socket.create_connection(self.server.server_address, timeout=1))
            with changed:
                self.assertTrue(changed.wait_for(lambda: active == self.server.MAX_CONNECTIONS, 1))
            with socket.create_connection(self.server.server_address, timeout=1) as overflow:
                self.assertEqual(overflow.recv(1), b"")
            self.assertEqual(active, self.server.MAX_CONNECTIONS)
        with changed:
            self.assertTrue(changed.wait_for(lambda: active == 0, 1))
        self.assertEqual(self.request("{}")[0], 400)


if __name__ == "__main__":
    unittest.main()
