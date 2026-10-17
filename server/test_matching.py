"""Socket regressions for real peer barriers, relays and cancellation."""
from contextlib import closing
import socket
import sqlite3
import struct
import tempfile
import threading
import unittest
from pathlib import Path

import piuserver as protocol
from test_piuserver import JoinedTCPServer, QuietTCP, receive_packet


class MatchingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.db = str(Path(self.directory.name) / "matching.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(protocol.SCHEMA)
        self.server = JoinedTCPServer(("127.0.0.1", 0), QuietTCP)
        self.server.db = self.db
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.addCleanup(self.stop)

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def connect_cabinet(self, number):
        client = socket.create_connection(self.server.server_address, timeout=3)
        self.addCleanup(client.close)
        registration = bytearray(0x394)
        struct.pack_into("<Q", registration, 4, number)
        client.sendall(protocol.encode_packet(1, 0x132003, registration))
        _, command, body = receive_packet(client)
        self.assertEqual(command, 0x132004)
        machine = struct.unpack_from("<I", body)[0]
        self.assertEqual(receive_packet(client)[1:], (0x13202d, struct.pack("<I", 1)))
        identity = struct.pack("<I8sI", machine, b"v2.12.0", 0)
        return client, identity

    def send(self, client, command, payload):
        client.sendall(protocol.encode_packet(5, command, payload))

    def pair(self):
        first, identity1 = self.connect_cabinet(10001)
        second, identity2 = self.connect_cabinet(10002)
        self.assertNotEqual(identity1[:4], identity2[:4])
        song = struct.pack("<3I", 0x1870, 0, 3)
        profile1 = b"FIRST".ljust(32, b"\0")
        profile2 = b"SECOND".ljust(32, b"\0")
        self.send(first, 0x132019, identity1 + profile1 + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(first)[1:], (0x13201a, b""))
        # A third client on another chart must not consume the first waiting peer.
        third, identity3 = self.connect_cabinet(10003)
        other_song = struct.pack("<3I", 0x1870, 0, 4)
        self.send(third, 0x132019, identity3 + bytes(32) + other_song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(third)[1:], (0x13201a, b""))
        self.send(second, 0x132019, identity2 + profile2 + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(second)[1:], (0x13201a, b""))
        self.assertEqual(receive_packet(first)[1:], (0x13201b, identity2 + profile2 + song))
        self.assertEqual(receive_packet(second)[1:], (0x13201b, identity1 + profile1 + song))
        return first, identity1, second, identity2

    def test_start_barrier_live_values_and_opponent_results(self):
        first, identity1, second, identity2 = self.pair()
        self.send(first, 0x13201c, identity1)
        self.send(second, 0x13201c, identity2)
        self.send(first, 0x13201d, identity1)
        self.assertEqual(receive_packet(first)[1:], (0x13201e, b""))
        first.settimeout(0.15)
        with self.assertRaises(socket.timeout):
            first.recv(1)
        first.settimeout(3)
        self.send(second, 0x13201d, identity2)
        self.assertEqual(receive_packet(second)[1:], (0x13201e, b""))
        self.assertEqual(receive_packet(first)[1:], (0x13201f, b""))
        self.assertEqual(receive_packet(second)[1:], (0x13201f, b""))
        live = struct.pack("<2H", 123, 456)
        self.send(first, 0x132021, live)
        self.assertEqual(receive_packet(second)[1:], (0x132021, live))
        result1 = struct.pack("<9I", 10, 20, 30, 40, 50, 60, 987654, 2, 70)
        result2 = struct.pack("<9I", 9, 19, 29, 39, 49, 59, 876543, 3, 69)
        self.send(first, 0x132022, identity1 + result1)
        self.assertEqual(receive_packet(first)[1:], (0x132023, b""))
        self.send(first, 0x132024, identity1)
        self.send(second, 0x132022, identity2 + result2)
        self.assertEqual(receive_packet(second)[1:], (0x132023, b""))
        self.assertEqual(receive_packet(first)[1:], (0x132025, result2))
        self.send(second, 0x132024, identity2)
        self.assertEqual(receive_packet(second)[1:], (0x132025, result1))
        # Starting the same song again is a fresh search, not an automatic
        # rematch against the stale opponent from the completed stage.
        song = struct.pack("<3I", 0x1870, 0, 3)
        profile1 = b"FIRST".ljust(32, b"\0")
        profile2 = b"SECOND".ljust(32, b"\0")
        self.send(first, 0x132019, identity1 + profile1 + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(second)[1:], (0x132029, b""))
        self.assertEqual(receive_packet(first)[1:], (0x13201a, b""))
        self.send(second, 0x132019, identity2 + profile2 + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(second)[1:], (0x13201a, b""))
        self.assertEqual(receive_packet(first)[1:], (0x13201b, identity2 + profile2 + song))
        self.assertEqual(receive_packet(second)[1:], (0x13201b, identity1 + profile1 + song))

    def test_cancellation_releases_both_peers(self):
        first, identity1, second, identity2 = self.pair()
        self.send(first, 0x132028, identity1)
        self.assertEqual(receive_packet(first)[1:], (0x132029, b""))
        self.assertEqual(receive_packet(second)[1:], (0x132029, b""))
        song = struct.pack("<3I", 0x1870, 0, 3)
        profile = b"REMATCH".ljust(32, b"\0")
        self.send(first, 0x132019, identity1 + profile + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(first)[1:], (0x13201a, b""))
        self.send(second, 0x132019, identity2 + profile + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(second)[1:], (0x13201a, b""))
        self.assertEqual(receive_packet(first)[1:], (0x13201b, identity2 + profile + song))
        self.assertEqual(receive_packet(second)[1:], (0x13201b, identity1 + profile + song))

    def test_photo_uploaded_before_search_is_available_when_peer_joins(self):
        first, identity1 = self.connect_cabinet(20001)
        second, identity2 = self.connect_cabinet(20002)
        photo = bytes(range(256)) * 32 + b"final-chunk"
        for index, offset in enumerate(range(0, len(photo), 8192)):
            chunk = photo[offset:offset + 8192]
            self.send(first, 0x13202e, struct.pack("<3I", len(chunk), len(photo), index) + chunk)
        song = struct.pack("<3I", 0x1870, 0, 3)
        profile = b"PHOTO".ljust(32, b"\0")
        self.send(first, 0x132019, identity1 + profile + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(first)[1:], (0x13201a, b""))
        self.send(second, 0x132019, identity2 + profile + song + struct.pack("<I", 1))
        self.assertEqual(receive_packet(second)[1:], (0x13201a, b""))
        self.assertEqual(receive_packet(first)[1], 0x13201b)
        self.assertEqual(receive_packet(second)[1], 0x13201b)
        _, command, token = receive_packet(second)
        self.assertEqual((command, token), (0x13202a, identity1[:4]))
        received = bytearray()
        for index in range(2):
            _, command, payload = receive_packet(second)
            size, total, part = struct.unpack_from("<3I", payload)
            self.assertEqual((command, total, part), (0x13202e, len(photo), index))
            self.assertEqual(len(payload), 12 + 8192)
            received.extend(payload[12:12 + size])
            self.assertEqual(payload[12 + size:], bytes(8192 - size))
        self.assertEqual(bytes(received), photo)


if __name__ == "__main__":
    unittest.main()
