"""Private protocol rejection and buffered-result ownership regressions.

Run: python -m unittest discover -s server -p 'test_*.py'
"""
from contextlib import closing
from pathlib import Path
import socket
import sqlite3
import struct
import tempfile
import threading
import unittest

import piuserver as protocol
import playerdata


class QuietTCP(protocol.TCP):
    def log(self, message):
        pass


class JoinedTCPServer(protocol.TCPServer):
    daemon_threads = False


def receive_packet(client):
    def read_exact(count):
        result = bytearray()
        while len(result) < count:
            chunk = client.recv(count - len(result))
            if not chunk:
                raise AssertionError("Unexpected connection close")
            result.extend(chunk)
        return bytes(result)

    header = read_exact(4)
    length = protocol.frame_length(struct.unpack("<I", header)[0])
    return protocol.decode_packet(header + read_exact(length - 4))


class PrivateSessionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.db = str(Path(self.directory.name) / "piu.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(protocol.SCHEMA)
        self.server = JoinedTCPServer(("127.0.0.1", 0), QuietTCP)
        self.server.db = self.db
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def connect(self):
        return socket.create_connection(self.server.server_address, timeout=3)

    def test_invalid_registration_never_dispatches(self):
        valid = protocol.encode_packet(1, 0x132003, bytes(0x394))
        for offset in (-64, -1, 4):
            with self.subTest(corruption_offset=offset), self.connect() as client:
                corrupted = bytearray(valid)
                corrupted[offset] ^= 1
                client.sendall(corrupted)
                self.assertEqual(client.recv(1), b"")
        with self.connect() as client:
            client.sendall(struct.pack("<I", protocol.MAX_FRAME + 1))
            self.assertEqual(client.recv(1), b"")
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT id FROM machines").fetchall(), [])

    def test_buffered_result_keeps_original_card_owner(self):
        with self.connect() as client:
            registration = protocol.encode_packet(1, 0x132003, bytes(0x394))
            # Split both the length word and packet body across socket writes.
            client.sendall(registration[:2])
            client.sendall(registration[2:40])
            client.sendall(registration[40:])
            self.assertEqual(receive_packet(client)[1], 0x132004)
            self.assertEqual(receive_packet(client)[1:],
                             (0x13202d, struct.pack("<I", 1)))

            profile_ids = []
            for sequence, card in enumerate((b"11223344", b"55667788"), 2):
                payload = struct.pack("<6I", 0, 1, 0, 0, 0, 1) + card.ljust(36, b"\0")
                heartbeat = protocol.encode_packet(50, 0x03000000, struct.pack("<I", 0xffff))
                client.sendall(protocol.encode_packet(sequence, 0x132005, payload) + heartbeat)
                _, command, body = receive_packet(client)
                self.assertEqual(command, 0x132006)
                profile_ids.append(struct.unpack_from("<I", body, 0x110 + 0x3c)[0])
                self.assertEqual(receive_packet(client)[1:],
                                 (0x132007, struct.pack("<2I", 0, 1)))
                self.assertEqual(receive_packet(client),
                                 (50, 0x03000000, struct.pack("<I", 0xffff)))
            self.assertNotEqual(*profile_ids)

            result = bytearray(108)
            struct.pack_into("<I", result, 0, 1870)
            struct.pack_into("<I", result, 12, 987654)
            struct.pack_into("<2I", result, 24, 1, profile_ids[0])
            result[0x22] = 1  # Buffered result from the first card.
            client.sendall(protocol.encode_packet(4, 0x13200a, result))
            self.assertEqual(receive_packet(client)[1:], (0x13200b, b""))

        with closing(sqlite3.connect(self.db)) as connection:
            stored = connection.execute(
                "SELECT users.card_id,scores.song_id,scores.score FROM scores "
                "JOIN users ON users.id=scores.user_id").fetchall()
        self.assertEqual(stored, [("11223344", "1870", 987654)])
        first_id, first_profile = playerdata.load_profile(self.db, "11223344")
        self.assertEqual(first_id, profile_ids[0])
        self.assertEqual(struct.unpack_from("<I", first_profile, 0x3c)[0], first_id)


if __name__ == "__main__":
    unittest.main()
