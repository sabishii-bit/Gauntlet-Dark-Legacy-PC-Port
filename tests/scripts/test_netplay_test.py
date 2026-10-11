import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import netplay_test


class TransportHarnessTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.log = self.root / "test.log"

    def write(self, rows):
        self.log.write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")

    def test_exit_without_exchange_is_not_a_pass(self):
        self.write([{"event": "passed", "role": "client", "ticks": 120, "ordered_controls": 15}])
        netplay_test.verify_log(self.log, "client", 0)
        for code in (1, -1):
            with self.assertRaises(RuntimeError):
                netplay_test.verify_log(self.log, "client", code)
        for ticks, controls in ((0, 15), (120, 0)):
            self.write([{"event": "passed", "role": "client", "ticks": ticks, "ordered_controls": controls}])
            with self.assertRaises(RuntimeError):
                netplay_test.verify_log(self.log, "client", 0)

    def test_host_requires_each_distinct_peer(self):
        good = [{"event": "passed", "role": "host", "clients": 3},
                *[{"event": "peer_done", "seat": seat, "accepted": 120} for seat in range(3)]]
        self.write(good)
        netplay_test.verify_log(self.log, "host", 0)
        for bad in (good[:-1], good + [good[0]], good[:-1] + [good[1]]):
            self.write(bad)
            with self.assertRaises(RuntimeError):
                netplay_test.verify_log(self.log, "host", 0)

    def test_private_invitations_never_belong_in_diagnostic_logs(self):
        self.write([{"event": "passed", "role": "self-test", "secret": "gdl1-private"}])
        with self.assertRaises(RuntimeError):
            netplay_test.verify_log(self.log, "self-test", 0)

    def test_stop_reaps_a_stuck_child(self):
        child = mock.Mock()
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired("child", 3), 0]
        netplay_test.stop(child)
        child.terminate.assert_called_once()
        child.kill.assert_called_once()
        self.assertEqual(child.wait.call_count, 2)

    def test_invitation_wait_detects_host_failure(self):
        child = mock.Mock()
        child.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            netplay_test.wait_invitation(child, self.root / "invite.txt", self.log)


if __name__ == "__main__":
    unittest.main()
