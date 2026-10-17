#!/usr/bin/env python3
"""Phoenix LAN HTTP and native game-session service.

Ordinary packets retain magic, RC4 and SHA-1 verification, with an untransformed
64-byte trailer. The explicit private-server client transport is required.
Cabinets, players, results, native best records and transfers persist in SQLite.
"""
import argparse
from contextlib import closing
import datetime
import http.server
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import socketserver
import sqlite3
import struct
import threading
import urllib.parse

import gameplay
import matching
import playerdata
import webui

SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS users (
 id INTEGER PRIMARY KEY, card_id TEXT NOT NULL UNIQUE, nickname TEXT NOT NULL,
 avatar_settings BLOB, created TEXT NOT NULL, last_login TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS profiles (
 id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
 version INTEGER NOT NULL, save_data BLOB NOT NULL, parsed_fields TEXT NOT NULL,
 timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS scores (
 id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
 song_id TEXT NOT NULL, chart TEXT NOT NULL, score INTEGER NOT NULL,
 grade TEXT, judgements TEXT, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS machines (
 id TEXT PRIMARY KEY, info BLOB NOT NULL, registered TEXT NOT NULL,
 last_seen TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS requests (
 id INTEGER PRIMARY KEY, method TEXT NOT NULL, path TEXT NOT NULL,
 body BLOB NOT NULL, status INTEGER NOT NULL, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tcp_packets (
 id INTEGER PRIMARY KEY, connection INTEGER NOT NULL, direction TEXT NOT NULL,
 data BLOB NOT NULL, timestamp TEXT NOT NULL
);
""" + gameplay.SCHEMA + playerdata.PROFILE_SCHEMA

MAGIC = bytes.fromhex("3753ae51b73d865827eb86fa")
MAX_FRAME = 0x2820
ORDINARY_OVERHEAD = 0x4c
SPEED_TEST_SIZE = 500 * 1024 * 1024  # Actual LAN transfer size, not vendor metadata.


class ProtocolError(ValueError):
    pass


def rc4(key, data):
    if len(key) != 32:
        raise ProtocolError("RC4 requires a 32-byte packet key")
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % 32]) & 255
        state[i], state[j] = state[j], state[i]
    output = bytearray(len(data))
    i = j = 0
    for position, value in enumerate(data):
        i = (i + 1) & 255
        j = (j + state[i]) & 255
        state[i], state[j] = state[j], state[i]
        output[position] = value ^ state[(state[i] + state[j]) & 255]
    return bytes(output)


def frame_length(length):
    if length != 16 and not ORDINARY_OVERHEAD <= length <= MAX_FRAME:
        raise ProtocolError(f"invalid frame length {length}")
    return length


def decode_packet(frame):
    """Return (sequence, command, payload), rejecting before command dispatch."""
    if len(frame) < 4:
        raise ProtocolError("missing length word")
    length = frame_length(struct.unpack_from("<I", frame)[0])
    if len(frame) != length:
        raise ProtocolError("frame length does not match the length word")
    if length == 16:
        sequence, command = struct.unpack_from("<2I", frame, 4)
        return sequence, command, frame[12:]
    trailer = frame[-64:]
    if not hmac.compare_digest(trailer[:12], MAGIC):
        raise ProtocolError("invalid trailer magic")
    plain = rc4(trailer[12:44], frame[4:-64])
    if not hmac.compare_digest(hashlib.sha1(plain).digest(), trailer[44:]):
        raise ProtocolError("invalid packet SHA-1")
    sequence, command = struct.unpack_from("<2I", plain)
    return sequence, command, plain[8:]


def encode_packet(sequence, command, payload=b""):
    """Use the original four-byte-payload plaintext exception, otherwise RC4."""
    if not 0 <= sequence <= 0xffffffff or not 0 <= command <= 0xffffffff:
        raise ProtocolError("sequence or command outside u32")
    if len(payload) == 4:
        return struct.pack("<3I", 16, sequence, command) + payload
    length = frame_length(len(payload) + ORDINARY_OVERHEAD)
    plain = struct.pack("<2I", sequence, command) + payload
    key = os.urandom(32)
    trailer = MAGIC + key + hashlib.sha1(plain).digest()
    return struct.pack("<I", length) + rc4(key, plain) + trailer


class PacketStream:
    """Bounded frame accumulator, independent of recv chunk boundaries."""
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        packets = []
        consumed = 0
        while len(self.pending) - consumed >= 4:
            length = frame_length(struct.unpack_from("<I", self.pending, consumed)[0])
            if len(self.pending) - consumed < length:
                break
            packets.append(decode_packet(bytes(self.pending[consumed:consumed + length])))
            consumed += length
        if consumed:
            del self.pending[:consumed]
        return packets


def require_size(payload, size):
    if len(payload) != size:
        raise ProtocolError(f"payload length {len(payload)}; expected {size}")


def require_player(player):
    if player not in (0, 1):
        raise ProtocolError(f"invalid player index {player}")


def card_text(field):
    text, terminator, _ = field.partition(b"\0")
    if (not terminator or not 2 <= len(text) <= 34 or len(text) % 2 or
            any(value not in b"0123456789abcdefABCDEF" for value in text)):
        raise ProtocolError("card ID must be NUL-terminated, even-length hex (2-34 characters)")
    return text.decode("ascii").lower()




def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def record(db, sql, values):
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        cursor = connection.execute(sql, values)
        return cursor.lastrowid


class HTTP(http.server.BaseHTTPRequestHandler):
    server_version = "piuserver/1"

    def log_message(self, fmt, *args):
        pass  # One structured line below includes the body and response status.

    def respond(self):
        if webui.handle(self):
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "Invalid Content-Length")
            return
        if not 0 <= length <= 16 * 1024 * 1024:
            self.send_error(413, "Request body too large")
            return
        body = self.rfile.read(length)
        path = urllib.parse.urlsplit(self.path).path
        asset_root = getattr(self.server, "assets", None)
        stream_size, asset = None, None
        status, content_type = 404, "text/plain; charset=utf-8"
        response = b"Unidentified Phoenix endpoint; no synthetic success response.\n"
        if self.command == "GET" and path == "/piu.module/piu.get_ip_to_country.php":
            # The binary reads country_no as cJSON valuestring, not a JSON number.
            response = json.dumps({"country_data": {
                "country_no": str(self.server.country), "ip": self.client_address[0]
            }}, separators=(",", ":")).encode()
            status, content_type = 200, "application/json"
        elif self.command == "GET" and path == "/ip/":
            response, status = self.client_address[0].encode("ascii"), 200
        elif self.command == "GET" and path == "/dummy/test500mb.db":
            # Native 0x808192c discards bytes; progress uses HTTP Content-Length.
            stream_size, status = SPEED_TEST_SIZE, 200
            content_type, response = "application/octet-stream", b""
        elif self.command == "GET" and asset_root and path.startswith(
                ("/assets/", "/2023/ad/", "/2023/update/")):
            relative = urllib.parse.unquote(
                path[len("/assets/"):] if path.startswith("/assets/") else path.lstrip("/"))
            root = Path(asset_root).resolve()
            asset = (root / relative).resolve()
            if asset.is_relative_to(root) and asset.is_file():
                stream_size, status = asset.stat().st_size, 200
                content_type, response = "application/octet-stream", b""
            else:
                asset = None
        record(self.server.db,
               "INSERT INTO requests(method,path,body,status,timestamp) VALUES(?,?,?,?,?)",
               (self.command, self.path, body, status, now()))
        summary = repr(body[:256]) + ("..." if len(body) > 256 else "")
        print(f"{now()} HTTP {self.command} {self.path} bytes={len(body)} "
              f"body={summary} -> {status} response_bytes={stream_size if stream_size is not None else len(response)}",
              flush=True)
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(stream_size if stream_size is not None else len(response)))
        self.send_header("Connection", "close")
        self.end_headers()
        if stream_size is None:
            self.wfile.write(response)
        else:
            try:
                if asset is not None:
                    with asset.open("rb") as source:
                        while chunk := source.read(1024 * 1024):
                            self.wfile.write(chunk)
                else:
                    chunk = memoryview(bytes(1024 * 1024))
                    remaining = stream_size
                    while remaining:
                        count = min(remaining, len(chunk))
                        self.wfile.write(chunk[:count])
                        remaining -= count
            except (BrokenPipeError, ConnectionResetError):
                pass  # Native diagnostics allow cancellation during a transfer.

    do_GET = respond
    do_POST = respond


class TCP(socketserver.BaseRequestHandler):
    def log(self, message):
        print(f"{now()} TCP connection={self.connection} {message}", flush=True)

    def send_packet(self, command, payload=b"", sequence=None):
        with self.send_lock:
            if sequence is None:
                self.sequence = (self.sequence + 1) & 0xffffffff
                sequence = self.sequence
            frame = encode_packet(sequence, command, payload)
            self.request.sendall(frame)
            record(self.server.db,
                   "INSERT INTO tcp_packets(connection,direction,data,timestamp) VALUES(?,?,?,?)",
                   (self.connection, "send", frame, now()))
            self.log(f"reply seq={sequence} command=0x{command:08x} payload={len(payload)}")

    def machine(self, command, payload):
        require_size(payload, 0x318 if command == 0x132001 else 0x394)
        self.machine_id, identifier_text = gameplay.cabinet(
            self.server.db, payload, self.client_address[0], 8 if command == 0x132001 else 0)
        identifier = identifier_text.encode("ascii").ljust(12, b"\0")
        if command == 0x132001:
            response = struct.pack("<2I12s2I", 0, 0, identifier, self.machine_id, 0)
            reply = 0x132002
        else:
            description = b"Laomei De NASA Zheme Lihai?".ljust(72, b"\0")
            response = struct.pack("<2I", self.machine_id, 0) + description + identifier
            reply = 0x132004
        self.registered = True
        self.send_packet(reply, response)
        self.send_packet(0x13202d, struct.pack("<I", 1))
        # Native event definitions are pushed, not fetched by an invented URL.
        with closing(sqlite3.connect(self.server.db)) as connection:
            definitions = list(gameplay.event_definitions(connection, self.machine_id))
            earned_charts = list(gameplay.earned_chart_packets(connection, self.machine_id))
            messages = connection.execute(
                "SELECT revision,enabled,path FROM local_messages ORDER BY slot").fetchall()
        for definition in definitions:
            self.send_packet(0x132032, definition)
        for rewards in earned_charts:
            self.send_packet(0x132030, rewards)
        if any(row[1] for row in messages):
            self.send_packet(0x132014, struct.pack("<4I", messages[0][0], messages[1][0],
                             messages[0][1], messages[1][1]) +
                             b"".join(row[2].encode("utf-8")[:63].ljust(64, b"\0")
                                      for row in messages))
        self.log(f"machine id={self.machine_id} identifier={identifier_text}")

    def require_machine(self, machine_id):
        if not self.registered or machine_id != self.machine_id:
            raise ProtocolError(f"unregistered LAN machine {machine_id}")

    def card(self, payload):
        require_size(payload, 0x3c)
        player, machine_id = struct.unpack_from("<2I", payload)
        require_player(player)
        self.require_machine(machine_id)
        card_id = card_text(payload[24:])
        # A new card session must not inherit the previous player's match/photo.
        self.server.matching.disconnected(self)
        user_id, profile = playerdata.load_profile(self.server.db, card_id)
        self.players[player] = user_id
        timestamp = now()
        with closing(sqlite3.connect(self.server.db)) as connection, connection:
            connection.execute(
                "UPDATE player_sessions SET ended=? WHERE connection=? AND player=? AND ended IS NULL",
                (timestamp, self.connection, player))
            connection.execute(
                "INSERT INTO player_sessions(connection,player,user_id,machine_id,started) VALUES(?,?,?,?,?)",
                (self.connection, player, user_id, self.machine_id, timestamp))
        # 0x080654f6 copies the record, sets player state 5 when no resource path
        # is present, and stores the two associated words at R+0x68/R+0x64.
        # They have no mapped semantics; an empty local profile keeps both 0.
        response = struct.pack("<4I", player, 0, 0, 0) + bytes(256) + profile
        self.send_packet(0x132006, response)
        for page in gameplay.history_pages(self.server.db, user_id, player):
            self.send_packet(0x132009, page)
        # 0x08087800 explicitly requires status 1 to finish ordinary loading.
        self.send_packet(0x132007, struct.pack("<2I", player, 1))
        self.log(f"card player={player} card={card_id} local_profile={user_id}")

    def result(self, payload):
        require_size(payload, 0x6c)
        player = struct.unpack_from("<I", payload, 0x60)[0]
        require_player(player)
        machine_id, user_id = struct.unpack_from("<2I", payload, 0x18)
        self.require_machine(machine_id)
        # Queued results can belong to a previous card/session. Resolve the
        # transmitted profile ID, not whichever player most recently logged in.
        song_id = struct.unpack_from("<I", payload)[0]
        score = struct.unpack_from("<I", payload, 0x0c)[0]
        chart = {"selector": payload[4], "mode_bits": payload[5]}
        details = {
            "counters_0x24": list(struct.unpack_from("<5I", payload, 0x24)),
            "max_combo_0x38": struct.unpack_from("<I", payload, 0x38)[0],
            "flags_0x06": payload[6],
            "category_0x08": payload[8],
            "country": struct.unpack_from("<H", payload, 0x20)[0],
            "buffered": payload[0x22],
            "player": player,
            "completion_context": list(struct.unpack_from("<2I", payload, 0x64)),
            "raw": payload.hex(),
        }
        with closing(sqlite3.connect(self.server.db)) as connection, connection:
            connection.execute("PRAGMA foreign_keys=ON")
            if user_id == 0:
                result_id = connection.execute(
                    "INSERT INTO guest_results(machine_id,player,payload,timestamp) VALUES(?,?,?,?)",
                    (machine_id, player, payload, now())).lastrowid
            else:
                if not connection.execute("SELECT 1 FROM users WHERE id=?", (user_id,)).fetchone():
                    self.log(f"unmapped result profile={user_id}; recorded, not acknowledged")
                    return
                cursor = connection.execute(
                    "INSERT INTO scores(user_id,song_id,chart,score,grade,judgements,timestamp) "
                    "VALUES(?,?,?,?,?,?,?)",
                    (user_id, str(song_id), json.dumps(chart, separators=(",", ":")),
                     score, f"0x{payload[7]:02x}", json.dumps(details, separators=(",", ":")), now()))
                result_id = cursor.lastrowid
                connection.execute(
                    "INSERT INTO result_packets(score_id,machine_id,player,payload) VALUES(?,?,?,?)",
                    (result_id, machine_id, player, payload))
                gameplay.save_best(connection, user_id, payload)
            event_updates = gameplay.advance_events(connection, machine_id, user_id, payload)
            connection.execute(
                "UPDATE player_sessions SET result_count=result_count+1 WHERE connection=? AND "
                "player=? AND user_id IS ? AND ended IS NULL",
                (self.connection, player, user_id or None))
        if user_id:
            playerdata.save_result_options(self.server.db, user_id, payload)
        # 0x080869f7-0x08086ae5 only resets timing/counters; it never reads a
        # response body. A framed empty body is sufficient for this client.
        self.send_packet(0x13200b)
        for event_id, progress in event_updates:
            if progress < 100:
                self.send_packet(0x132035, struct.pack("<2If", event_id, self.machine_id, progress))
        self.log(f"result id={result_id} player={player} local_profile={user_id} "
                 f"song={song_id} chart={chart} score={score}")

    def activity(self, command, payload):
        sizes = {0x13200c: 12, 0x13200d: 20, 0x13200f: 40,
                 0x132011: 36, 0x132015: 528, 0x13203b: 48}
        require_size(payload, sizes[command])
        if command != 0x132011:
            machine_offset = 4 if command == 0x132015 else 0
            self.require_machine(struct.unpack_from("<I", payload, machine_offset)[0])
        elif not self.registered:
            raise ProtocolError("update provisioning before cabinet registration")
        record(self.server.db,
               "INSERT INTO cabinet_activity(machine_id,command,payload,timestamp) VALUES(?,?,?,?)",
               (self.machine_id, command, payload, now()))
        if command == 0x13200d:
            machine, user_id, player, pricing, context = struct.unpack("<5I", payload)
            require_player(player)
            # The native transition is one-way; preserve its real context word.
            with closing(sqlite3.connect(self.server.db)) as connection, connection:
                row = connection.execute(
                    "SELECT id FROM player_sessions WHERE connection=? AND player=? AND ended IS NULL",
                    (self.connection, player)).fetchone()
                if row:
                    connection.execute("UPDATE player_sessions SET last_transition=? WHERE id=?",
                                       (payload, row[0]))
                elif user_id == 0:
                    connection.execute(
                        "INSERT INTO player_sessions(connection,player,machine_id,started,last_transition) "
                        "VALUES(?,?,?,?,?)", (self.connection, player, machine, now(), payload))

    def reward(self, payload):
        require_size(payload, 20)
        player, user_id = struct.unpack_from("<2I", payload)
        require_player(player)
        if self.players.get(player) != user_id:
            raise ProtocolError("reward request does not match the logged-in player")
        with closing(sqlite3.connect(self.server.db)) as connection, connection:
            reward = connection.execute(
                "SELECT title_id FROM title_rewards WHERE user_id=? AND notified=0 ORDER BY awarded,title_id LIMIT 1",
                (user_id,)).fetchone()
            connection.execute(
                "UPDATE player_sessions SET ended=? WHERE connection=? AND player=? AND user_id=? AND ended IS NULL",
                (now(), self.connection, player, user_id))
        # Status 1 is only sent with an earned local title; 0 displays nothing.
        self.send_packet(0x132017, struct.pack("<5I", player, 0, int(reward is not None),
                                             0, reward[0] if reward else 0))
        self.send_packet(0x132018, struct.pack("<I", player))
        if reward:
            record(self.server.db, "UPDATE title_rewards SET notified=1 WHERE user_id=? AND title_id=?",
                   (user_id, reward[0]))

    def event(self, command, payload):
        require_size(payload, 4 if command == 0x132036 else 8)
        notification = None
        if command == 0x132036:
            if not self.registered:
                raise ProtocolError("event reward before cabinet registration")
            event_id = struct.unpack("<I", payload)[0]
        else:
            machine_id, event_id = struct.unpack("<2I", payload)
            self.require_machine(machine_id)
        with closing(sqlite3.connect(self.server.db)) as connection, connection:
            event = connection.execute(
                "SELECT title_id FROM local_events WHERE id=? AND enabled=1", (event_id,)).fetchone()
            progress = gameplay.event_percent(connection, event_id, self.machine_id)
            if command == 0x132033:
                response = struct.pack("<IfI", event_id, progress, int(event is not None))
                reply = 0x132034
            elif command == 0x132036:
                player, earned = 0, False
                if event and event[0] is not None and progress >= 100:
                    for active_player, user_id in connection.execute(
                            "SELECT player,user_id FROM player_sessions "
                            "WHERE connection=? AND ended IS NULL AND user_id IS NOT NULL ORDER BY player",
                            (self.connection,)):
                        connection.execute(
                            "INSERT OR IGNORE INTO title_rewards(user_id,title_id,event_id,awarded) VALUES(?,?,?,?)",
                            (user_id, event[0], event_id, now()))
                        pending = connection.execute(
                            "SELECT 1 FROM title_rewards WHERE user_id=? AND title_id=? AND notified=0",
                            (user_id, event[0])).fetchone()
                        if pending and notification is None:
                            player, earned = active_player, True
                            notification = (user_id, event[0])
                    connection.execute(
                        "UPDATE event_progress SET claimed=1 WHERE event_id=? AND machine_id=?",
                        (event_id, self.machine_id))
                # 0x8085cb8 reads the recipient at +4, not the reserved +0 word.
                response = struct.pack("<4I", 0, player, int(earned), event[0] if earned else 0)
                reply = 0x132037
            else:
                charts = connection.execute(
                    "SELECT song_id,mode,selector FROM event_rewards WHERE event_id=? ORDER BY song_id,mode,selector",
                    (event_id,)).fetchall() if event and progress >= 100 else []
                response = gameplay.chart_packet(charts)
                reply = 0x132030
        self.send_packet(reply, response)
        if command == 0x13202f and progress >= 100:
            self.send_packet(0x132035, struct.pack("<2If", event_id, self.machine_id, progress))
        if notification:
            record(self.server.db, "UPDATE title_rewards SET notified=1 WHERE user_id=? AND title_id=?",
                   notification)

    def screenshot(self, payload):
        if not self.registered or len(payload) < 12:
            raise ProtocolError("screenshot before registration or missing chunk header")
        length, total, index = struct.unpack_from("<3I", payload)
        if (not 0 < length <= 8192 or not 0 < total <= 81920 or
                len(payload) not in (12 + length, 8204) or index * 8192 + length > total or
                (index * 8192 + length != total and length != 8192)):
            raise ProtocolError("invalid native screenshot chunk")
        with closing(sqlite3.connect(self.server.db)) as connection, connection:
            if index == 0:
                self.screenshot_id = connection.execute(
                    "INSERT INTO screenshots(connection,machine_id,total_size,timestamp) VALUES(?,?,?,?)",
                    (self.connection, self.machine_id, total, now())).lastrowid
            if self.screenshot_id is None:
                raise ProtocolError("screenshot chunk without a starting chunk")
            saved = connection.execute(
                "SELECT total_size FROM screenshots WHERE id=?", (self.screenshot_id,)).fetchone()
            if saved[0] != total:
                raise ProtocolError("screenshot total changed during transfer")
            connection.execute(
                "INSERT INTO screenshot_chunks(screenshot_id,chunk_index,data) VALUES(?,?,?) "
                "ON CONFLICT(screenshot_id,chunk_index) DO UPDATE SET data=excluded.data",
                (self.screenshot_id, index, payload[12:12 + length]))
            chunks = connection.execute(
                "SELECT chunk_index,data FROM screenshot_chunks WHERE screenshot_id=? ORDER BY chunk_index",
                (self.screenshot_id,)).fetchall()
            if [row[0] for row in chunks] == list(range(len(chunks))):
                image = b"".join(row[1] for row in chunks)
                if len(image) == total:
                    connection.execute("UPDATE screenshots SET image=? WHERE id=?",
                                       (image, self.screenshot_id))
        self.server.matching.screenshot(self, payload)

    def dispatch(self, sequence, command, payload):
        self.log(f"request seq={sequence} command=0x{command:08x} payload={len(payload)}")
        record(self.server.db,
               "INSERT INTO decoded_packets(connection,sequence,command,payload,timestamp) VALUES(?,?,?,?,?)",
               (self.connection, sequence, command, payload, now()))
        if command == 0x03000000:
            require_size(payload, 4)
            self.send_packet(command, payload, sequence=sequence)
        elif command in (0x132001, 0x132003):
            self.machine(command, payload)
        elif command == 0x132005:
            self.card(payload)
        elif command == 0x13200a:
            self.result(payload)
        elif command == 0x13200e:
            require_size(payload, 44)
            machine_id, user_id = struct.unpack_from("<2I", payload)
            self.require_machine(machine_id)
            playerdata.save_prefix(self.server.db, user_id, payload[8:])
        elif command in (0x13200c, 0x13200d, 0x13200f, 0x132015, 0x13203b):
            self.activity(command, payload)
        elif command == 0x132011:
            require_size(payload, 36)
            # 0x8065134 writes a hardware-protected update key, not a status.
            # No locally generated bytes can stand in for a licensed key.
            self.activity(command, payload)
            key = Path(getattr(self.server, "assets", "server/assets")) / "key_phoenix_2120.bin"
            if key.is_file() and key.stat().st_size == 1024:
                self.send_packet(0x132012, key.read_bytes())
            else:
                self.log("update key requested; missing exact 1024-byte operator provisioning asset")
        elif command == 0x132016:
            self.reward(payload)
        elif command in (0x13202f, 0x132033, 0x132036):
            self.event(command, payload)
        elif command == 0x13202e:
            self.screenshot(payload)
        elif self.server.matching.dispatch(self, command, payload):
            return
        else:
            self.log(f"unknown command=0x{command:08x}; recorded, unanswered")

    def handle(self):
        self.request.settimeout(30)
        self.connection = record(
            self.server.db,
            "INSERT INTO tcp_packets(connection,direction,data,timestamp) "
            "VALUES(0,'connect',?,?)", (b"", now()))
        self.sequence = 0
        self.send_lock = threading.RLock()
        self.registered = False
        self.machine_id = None
        self.players = {}
        self.screenshot_id = None
        self.log(f"connect peer={self.client_address}")
        stream = PacketStream()
        try:
            while True:
                try:
                    data = self.request.recv(65536)
                except socket.timeout:
                    if stream.pending:
                        raise ProtocolError("timed out with an incomplete frame")
                    continue  # A song may last much longer than an idle recv.
                if not data:
                    if stream.pending:
                        raise ProtocolError("connection closed with an incomplete frame")
                    return
                record(self.server.db,
                       "INSERT INTO tcp_packets(connection,direction,data,timestamp) VALUES(?,?,?,?)",
                       (self.connection, "receive", data, now()))
                for packet in stream.feed(data):
                    self.dispatch(*packet)
        except ValueError as error:
            self.log(f"rejected: {error}; closing")
        except (OSError, sqlite3.Error) as error:
            self.log(f"session error: {error}; closing")
        finally:
            self.server.matching.disconnected(self)
            record(self.server.db,
                   "UPDATE player_sessions SET ended=? WHERE connection=? AND ended IS NULL",
                   (now(), self.connection))


class TCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, *args, **kwargs):
        self.matching = matching.MatchingService()
        super().__init__(*args, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default="server/piu.db")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="HTTP and game listener bind address (LAN default: all interfaces)")
    parser.add_argument("--assets", default="server/assets",
                        help="Local files exposed beneath /assets/")
    parser.add_argument("--content", help="Optional local event/message JSON configuration")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--game-port", type=int, default=15040,
                        help="Local game-session TCP port; 0 disables it")
    parser.add_argument("--country", type=int, default=0)
    args = parser.parse_args()
    db = Path(args.db)
    db.parent.mkdir(parents=True, exist_ok=True)
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.executescript(SCHEMA)
        gameplay.migrate_results(connection)
    if args.content:
        gameplay.load_content(str(db), args.content)
    http_server = http.server.ThreadingHTTPServer((args.bind, args.port), HTTP)
    http_server.db, http_server.country = str(db), args.country
    http_server.assets = args.assets
    tcp = None
    try:
        if args.game_port:
            tcp = TCPServer((args.bind, args.game_port), TCP)
            tcp.db = str(db)
            tcp.assets = args.assets
            threading.Thread(target=tcp.serve_forever, daemon=True).start()
        print(f"piuserver HTTP {args.bind}:{args.port} db={db}; "
              f"LAN game TCP={args.game_port}", flush=True)
        http_server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        http_server.server_close()
        if tcp:
            tcp.shutdown()
            tcp.server_close()


if __name__ == "__main__":
    main()
