"""Native persistence boundaries not covered by codec or matching tests."""
from contextlib import closing
import json
from pathlib import Path
import socket
import sqlite3
import struct
import tempfile
import threading
import unittest

import gameplay
import piuserver as protocol
import playerdata
from test_piuserver import JoinedTCPServer, QuietTCP, receive_packet


class GameServicesTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.db = str(Path(self.directory.name) / "piu.db")
        with closing(sqlite3.connect(self.db)) as connection:
            connection.executescript(protocol.SCHEMA)
        self.server = JoinedTCPServer(("127.0.0.1", 0), QuietTCP)
        self.server.db = self.db
        self.server.assets = self.directory.name
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def connect(self):
        return socket.create_connection(self.server.server_address, timeout=3)

    def send(self, client, command, payload=b""):
        client.sendall(protocol.encode_packet(1, command, payload))

    def register(self, client, events=0):
        registration = bytearray(0x394)
        struct.pack_into("<Q", registration, 4, 123456)
        self.send(client, 0x132003, registration)
        _, command, payload = receive_packet(client)
        self.assertEqual(command, 0x132004)
        machine_id = struct.unpack_from("<I", payload)[0]
        self.assertEqual(receive_packet(client)[1:], (0x13202d, struct.pack("<I", 1)))
        for _ in range(events):
            self.assertEqual(receive_packet(client)[1], 0x132032)
        return machine_id

    def login(self, client, machine_id, player=0, card=b"11223344"):
        self.send(client, 0x132005,
                  struct.pack("<6I", player, machine_id, 0, 0, 0, 1) + card.ljust(36, b"\0"))
        _, command, payload = receive_packet(client)
        self.assertEqual(command, 0x132006)
        return struct.unpack_from("<I", payload, 0x14c)[0], payload[0x110:]

    def result(self, machine_id, user_id, score=987654, grade=2, category=3):
        payload = bytearray(108)
        struct.pack_into("<I", payload, 0, 1870)
        payload[4:9] = bytes((15, 64, 0, grade, category))
        struct.pack_into("<I", payload, 12, score)
        struct.pack_into("<2I", payload, 24, machine_id, user_id)
        struct.pack_into("<2I2HIf", payload, 0x40, 0x1234, 0x5678, 0x400, 9, 675, 2.0)
        return payload

    def barrier(self, client):
        self.send(client, 0x03000000, struct.pack("<I", 0xffff))
        self.assertEqual(receive_packet(client)[1:], (0x03000000, struct.pack("<I", 0xffff)))

    def test_result_best_options_and_prefix_reload(self):
        with self.connect() as client:
            machine_id = self.register(client)
            user_id, original = self.login(client, machine_id)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            first = self.result(machine_id, user_id)
            second = self.result(machine_id, user_id, score=123, grade=10, category=1)
            for payload in (first, second):
                self.send(client, 0x13200a, payload)
                self.assertEqual(receive_packet(client)[1:], (0x13200b, b""))
            prefix = bytes(range(36))
            self.send(client, 0x13200e, struct.pack("<2I", machine_id, user_id) + prefix)
            self.barrier(client)
        with self.connect() as client:
            self.assertEqual(self.register(client), machine_id)
            same_id, loaded = self.login(client, machine_id)
            self.assertEqual(same_id, user_id)
            self.assertEqual(loaded[:36], prefix)
            self.assertEqual(loaded[0x24:0x4c], original[0x24:0x4c])
            self.assertEqual(loaded[0x4c:0x58], second[0x40:0x4c])
            self.assertEqual(struct.unpack_from("<H", loaded, 0x58)[0], 675)
            self.assertEqual(struct.unpack_from("<f", loaded, 0x5c)[0], 2.0)
            _, command, history = receive_packet(client)
            self.assertEqual(command, 0x132009)
            self.assertEqual(len(history), 6160)
            self.assertEqual(struct.unpack_from("<4I", history), (0, 0, 0, 1))
            record = history[16:40]
            self.assertEqual(struct.unpack_from("<I", record)[0], 1870)
            self.assertEqual(record[4:9], bytes((15, 64, 0, 2, 1)))
            self.assertEqual(struct.unpack_from("<2I", record, 12), (987654, 2))
            self.assertEqual(receive_packet(client)[1], 0x132007)
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM scores").fetchone()[0], 2)
            self.assertEqual(connection.execute("SELECT payload FROM result_packets ORDER BY score_id").fetchall(),
                             [(bytes(first),), (bytes(second),)])

    def test_screenshot_reassembly_preserves_every_chunk(self):
        image = bytes(range(256)) * 35
        with self.connect() as client:
            self.register(client)
            for index, start in enumerate(range(0, len(image), 8192)):
                chunk = image[start:start + 8192]
                self.send(client, 0x13202e, struct.pack("<3I", len(chunk), len(image), index) + chunk)
            self.barrier(client)
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT image FROM screenshots").fetchone()[0], image)
            self.assertEqual(connection.execute("SELECT count(*) FROM screenshot_chunks").fetchone()[0], 2)

    def test_local_event_goal_and_real_title_notification(self):
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute(
                "INSERT INTO local_events(id,sprite,target_plays,title_id) VALUES(7,'LAN_GOAL',2,42)")
            connection.execute("INSERT INTO event_charts VALUES(7,1870,64,15)")
            connection.execute("INSERT INTO event_rewards VALUES(7,1870,64,15)")
            connection.execute(
                "INSERT INTO local_events(id,sprite,target_plays,title_id) VALUES(8,'LAN_GOAL_TWO',2,43)")
            connection.execute("INSERT INTO event_charts VALUES(8,1870,64,15)")
        with self.connect() as client:
            machine_id = self.register(client, events=2)
            user_id, _ = self.login(client, machine_id)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            for play in range(2):
                self.send(client, 0x13200a, self.result(machine_id, user_id))
                self.assertEqual(receive_packet(client)[1], 0x13200b)
                if play == 0:
                    self.assertEqual(receive_packet(client)[1], 0x132035)
                    self.assertEqual(receive_packet(client)[1], 0x132035)
            self.send(client, 0x132033, struct.pack("<2I", machine_id, 7))
            _, command, payload = receive_packet(client)
            self.assertEqual(command, 0x132034)
            self.assertEqual(struct.unpack("<IfI", payload), (7, 100.0, 1))
            self.send(client, 0x132036, struct.pack("<I", 7))
            self.assertEqual(receive_packet(client)[1:], (0x132037, struct.pack("<4I", 0, 0, 1, 42)))
            self.send(client, 0x13202f, struct.pack("<2I", machine_id, 7))
            _, command, payload = receive_packet(client)
            self.assertEqual(command, 0x132030)
            self.assertEqual(struct.unpack_from("<I", payload, 12)[0], 1)
            self.assertEqual(struct.unpack_from("<3I", payload, 16), (1870, 64, 15))
            self.assertEqual(receive_packet(client)[1], 0x132035)
            self.send(client, 0x132016, struct.pack("<2I12s", 0, user_id, b"2.12.0"))
            self.assertEqual(receive_packet(client)[1:],
                             (0x132017, struct.pack("<5I", 0, 0, 1, 0, 43)))
            self.assertEqual(receive_packet(client)[1:], (0x132018, bytes(4)))
            self.send(client, 0x132016, struct.pack("<2I12s", 0, user_id, b"2.12.0"))
            self.assertEqual(receive_packet(client)[1:],
                             (0x132017, struct.pack("<5I", 0, 0, 0, 0, 0)))
            self.assertEqual(receive_packet(client)[1:], (0x132018, bytes(4)))
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute(
                "SELECT plays,claimed FROM event_progress ORDER BY event_id").fetchall(), [(2, 1), (2, 0)])
            self.assertEqual(connection.execute(
                "SELECT title_id,notified FROM title_rewards ORDER BY title_id").fetchall(), [(42, 1), (43, 1)])
        with self.connect() as client:
            self.assertEqual(self.register(client, events=2), machine_id)
            _, command, payload = receive_packet(client)
            self.assertEqual(command, 0x132030)
            self.assertEqual(struct.unpack_from("<I", payload, 12)[0], 1)
            self.assertEqual(struct.unpack_from("<3I", payload, 16), (1870, 64, 15))

    def test_event_title_targets_player_one_in_native_recipient_word(self):
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute(
                "INSERT INTO local_events(id,sprite,target_plays,title_id) VALUES(7,'LAN_GOAL',1,42)")
        with self.connect() as client:
            machine_id = self.register(client, events=1)
            user_id, _ = self.login(client, machine_id, player=1)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            result = self.result(machine_id, user_id)
            struct.pack_into("<I", result, 0x60, 1)
            self.send(client, 0x13200a, result)
            self.assertEqual(receive_packet(client)[1], 0x13200b)
            self.send(client, 0x132036, struct.pack("<I", 7))
            _, command, notification = receive_packet(client)
            self.assertEqual(command, 0x132037)
            # Native 0x8085cb8 reads +4, passed to the constructor at 0x8087980.
            self.assertEqual(struct.unpack_from("<3I", notification, 4), (1, 1, 42))

    def test_ended_account_does_not_receive_later_players_event_title(self):
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute(
                "INSERT INTO local_events(id,sprite,target_plays,title_id) VALUES(7,'LAN_GOAL',1,42)")
        with self.connect() as client:
            machine_id = self.register(client, events=1)
            departed_id, _ = self.login(client, machine_id)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            self.send(client, 0x132016, struct.pack("<2I12s", 0, departed_id, b"2.12.0"))
            self.assertEqual(receive_packet(client)[1], 0x132017)
            self.assertEqual(receive_packet(client)[1], 0x132018)
            active_id, _ = self.login(client, machine_id, player=1, card=b"55667788")
            self.assertEqual(receive_packet(client)[1], 0x132007)
            result = self.result(machine_id, active_id)
            struct.pack_into("<I", result, 0x60, 1)
            self.send(client, 0x13200a, result)
            self.assertEqual(receive_packet(client)[1], 0x13200b)
            self.send(client, 0x132036, struct.pack("<I", 7))
            self.assertEqual(receive_packet(client)[1], 0x132037)
            self.barrier(client)
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute(
                "SELECT user_id,title_id,notified FROM title_rewards").fetchall(),
                [(active_id, 42, 1)])

    def test_buffered_score_does_not_revert_newer_web_preferences(self):
        with self.connect() as client:
            machine_id = self.register(client)
            user_id, _ = self.login(client, machine_id)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            buffered = self.result(machine_id, user_id)
            buffered[0x22] = 1
            struct.pack_into("<I", buffered, 0x4c, 350)
            playerdata.update_player(self.db, user_id,
                                     {"speed": "12", "skin": "4", "auto_velocity": "700", "timing": "2"})
            self.send(client, 0x13200a, buffered)
            self.assertEqual(receive_packet(client)[1:], (0x13200b, b""))
            self.barrier(client)
        _, profile = playerdata.load_profile(self.db, "11223344")
        self.assertEqual(struct.unpack_from("<2H", profile, 0x54), (12, 4))
        self.assertEqual(struct.unpack_from("<H", profile, 0x58)[0], 700)
        self.assertEqual(struct.unpack_from("<f", profile, 0x5c)[0], 2)
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute(
                "SELECT user_id,score FROM scores").fetchall(), [(user_id, 987654)])
            self.assertEqual(struct.unpack_from("<I", connection.execute(
                "SELECT record FROM best_records").fetchone()[0], 12)[0], 987654)

    def test_native_session_end_player_then_profile_keeps_connection_alive(self):
        with self.connect() as client:
            machine_id = self.register(client)
            user_id, _ = self.login(client, machine_id)
            self.assertEqual(receive_packet(client)[1], 0x132007)
            # Captured game-origin 016 is player=0, profile=1, then 12 version
            # bytes. 0x8070e32/36 serialize EDX then ECX in that order.
            native_version = b"v2.12.0\0\0\0\0\xff"
            self.send(client, 0x132016, struct.pack("<2I12s", 0, user_id, native_version))
            self.assertEqual(receive_packet(client)[1:],
                             (0x132017, struct.pack("<5I", 0, 0, 0, 0, 0)))
            self.assertEqual(receive_packet(client)[1:], (0x132018, bytes(4)))
            self.barrier(client)

    def test_guest_result_does_not_create_fake_registered_player(self):
        with self.connect() as client:
            machine_id = self.register(client)
            self.send(client, 0x13200a, self.result(machine_id, 0))
            self.assertEqual(receive_packet(client)[1:], (0x13200b, b""))
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM users").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM guest_results").fetchone()[0], 1)

    def test_exact_operator_key_is_sent_not_synthesized(self):
        key = bytes(range(256)) * 4
        Path(self.directory.name, "key_phoenix_2120.bin").write_bytes(key)
        with self.connect() as client:
            self.register(client)
            self.send(client, 0x132011, b"piu_phoenix_2120.bin".ljust(32, b"\0") + bytes(4))
            self.assertEqual(receive_packet(client)[1:], (0x132012, key))

    def test_history_page_boundary_and_capture_migration(self):
        user_id, _ = playerdata.load_profile(self.db, "11223344")
        result = self.result(1, user_id)
        with closing(sqlite3.connect(self.db)) as connection, connection:
            connection.execute(
                "INSERT INTO scores(user_id,song_id,chart,score,judgements,timestamp) VALUES(?,?,?,?,?,?)",
                (user_id, "1870", "{}", 987654, json.dumps({"raw": result.hex()}), gameplay.now()))
            gameplay.migrate_results(connection)
            gameplay.migrate_results(connection)
            self.assertEqual(connection.execute("SELECT count(*) FROM result_packets").fetchone()[0], 1)
            for song_id in range(256):
                payload = bytearray(result)
                struct.pack_into("<I", payload, 0, song_id)
                gameplay.save_best(connection, user_id, payload)
        pages = list(gameplay.history_pages(self.db, user_id, 1))
        self.assertEqual([len(page) for page in pages], [6160, 6160])
        self.assertEqual(struct.unpack_from("<4I", pages[0]), (1, 0, 0, 256))
        self.assertEqual(struct.unpack_from("<4I", pages[1]), (1, 0, 256, 1))
        self.assertEqual(pages[1][40:], bytes(6120))

    def test_alternate_systemcheck_finds_same_cabinet_after_opaque_prefix(self):
        with self.connect() as client:
            machine_id = self.register(client)
            check = bytearray(0x318)
            struct.pack_into("<2I", check, 0, 0x12345678, 0x87654321)
            struct.pack_into("<IQ", check, 8, machine_id, 123456)
            self.send(client, 0x132001, check)
            _, command, response = receive_packet(client)
            self.assertEqual(command, 0x132002)
            self.assertEqual(len(response), 28)
            self.assertEqual(struct.unpack_from("<2I", response), (0, 0))
            self.assertEqual(struct.unpack_from("<I", response, 20)[0], machine_id)
            self.assertEqual(receive_packet(client)[1:], (0x13202d, struct.pack("<I", 1)))
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM cabinet_registry").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
