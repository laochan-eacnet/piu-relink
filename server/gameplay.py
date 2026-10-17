"""Mapped Phoenix records, cabinet identities and LAN-owned event data.

Wire records follow the original i386 consumers; event targets/rewards are local
operator policy, not reconstructed vendor schedules or hardware credentials.
"""
from contextlib import closing
import datetime
import json
import sqlite3
import struct

SCHEMA = """
CREATE TABLE IF NOT EXISTS cabinet_registry (
 id INTEGER PRIMARY KEY, identity TEXT NOT NULL UNIQUE,
 identifier TEXT NOT NULL UNIQUE, lock_number TEXT NOT NULL,
 peer TEXT NOT NULL, registered TEXT NOT NULL, last_seen TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS decoded_packets (
 id INTEGER PRIMARY KEY, connection INTEGER NOT NULL, sequence INTEGER NOT NULL,
 command INTEGER NOT NULL, payload BLOB NOT NULL, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS cabinet_activity (
 id INTEGER PRIMARY KEY, machine_id INTEGER NOT NULL, command INTEGER NOT NULL,
 payload BLOB NOT NULL, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS player_sessions (
 id INTEGER PRIMARY KEY, connection INTEGER NOT NULL, player INTEGER NOT NULL,
 user_id INTEGER REFERENCES users(id), machine_id INTEGER NOT NULL,
 started TEXT NOT NULL, ended TEXT, last_transition BLOB,
 result_count INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS best_records (
 user_id INTEGER NOT NULL REFERENCES users(id), song_id INTEGER NOT NULL,
 selector INTEGER NOT NULL, mode INTEGER NOT NULL, record BLOB NOT NULL,
 timestamp TEXT NOT NULL, PRIMARY KEY(user_id,song_id,selector,mode)
);
CREATE TABLE IF NOT EXISTS result_packets (
 score_id INTEGER PRIMARY KEY REFERENCES scores(id), machine_id INTEGER NOT NULL,
 player INTEGER NOT NULL, payload BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS guest_results (
 id INTEGER PRIMARY KEY, machine_id INTEGER NOT NULL, player INTEGER NOT NULL,
 payload BLOB NOT NULL, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS screenshots (
 id INTEGER PRIMARY KEY, connection INTEGER NOT NULL, machine_id INTEGER NOT NULL,
 total_size INTEGER NOT NULL, image BLOB, timestamp TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS screenshot_chunks (
 screenshot_id INTEGER NOT NULL REFERENCES screenshots(id),
 chunk_index INTEGER NOT NULL, data BLOB NOT NULL,
 PRIMARY KEY(screenshot_id,chunk_index)
);
CREATE TABLE IF NOT EXISTS local_events (
 id INTEGER PRIMARY KEY, word_04 INTEGER NOT NULL DEFAULT 0,
 word_08 INTEGER NOT NULL DEFAULT 0, word_0c INTEGER NOT NULL DEFAULT 0,
 sprite TEXT NOT NULL, target_plays INTEGER NOT NULL CHECK(target_plays>0),
 title_id INTEGER CHECK(title_id BETWEEN 0 AND 212), enabled INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS event_charts (
 event_id INTEGER NOT NULL REFERENCES local_events(id), song_id INTEGER NOT NULL,
 mode INTEGER NOT NULL, selector INTEGER NOT NULL,
 PRIMARY KEY(event_id,song_id,mode,selector)
);
CREATE TABLE IF NOT EXISTS event_rewards (
 event_id INTEGER NOT NULL REFERENCES local_events(id), song_id INTEGER NOT NULL,
 mode INTEGER NOT NULL, selector INTEGER NOT NULL,
 PRIMARY KEY(event_id,song_id,mode,selector)
);
CREATE TABLE IF NOT EXISTS event_progress (
 event_id INTEGER NOT NULL REFERENCES local_events(id), machine_id INTEGER NOT NULL,
 plays INTEGER NOT NULL DEFAULT 0, claimed INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(event_id,machine_id)
);
CREATE TABLE IF NOT EXISTS title_rewards (
 user_id INTEGER NOT NULL REFERENCES users(id), title_id INTEGER NOT NULL,
 event_id INTEGER NOT NULL, awarded TEXT NOT NULL, notified INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(user_id,title_id)
);
CREATE TABLE IF NOT EXISTS local_messages (
 slot INTEGER PRIMARY KEY CHECK(slot IN (0,1)), revision INTEGER NOT NULL DEFAULT 0,
 enabled INTEGER NOT NULL DEFAULT 0, path TEXT NOT NULL DEFAULT ''
);
INSERT OR IGNORE INTO local_messages(slot) VALUES(0),(1);
"""


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def cabinet(db, payload, peer, machine_offset=0):
    """Machine block: machine u32, lock number u64 (0x8061462).

    SYSTEMCHECK has an eight-byte caller prefix before that same block.
    """
    previous, lock_number = struct.unpack_from("<IQ", payload, machine_offset)
    timestamp = now()
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("BEGIN IMMEDIATE")
        saved = connection.execute(
            "SELECT id,identity,identifier,lock_number FROM cabinet_registry WHERE id=?", (previous,)).fetchone()
        if saved and saved[3] == str(lock_number):
            identity = saved[1]
        else:
            # Private clients may share a patched hardware number. Their LAN
            # addresses still distinguish cabinets; an assigned ID survives an
            # address change. Identical lock+peer+zero assigned ID is ambiguous.
            known = connection.execute(
                "SELECT identity FROM cabinet_registry WHERE lock_number=? AND peer=?",
                (str(lock_number), peer)).fetchone()
            identity = known[0] if known else f"lock:{lock_number}:peer:{peer}"
        row = connection.execute(
            "SELECT id,identifier FROM cabinet_registry WHERE identity=?", (identity,)).fetchone()
        if row:
            machine_id, identifier = row
            connection.execute("UPDATE cabinet_registry SET last_seen=?,peer=? WHERE id=?",
                               (timestamp, peer, machine_id))
        else:
            cursor = connection.execute(
                "INSERT INTO cabinet_registry(identity,identifier,lock_number,peer,registered,last_seen) "
                "VALUES(?,lower(hex(randomblob(6))),?,?,?,?)",
                (identity, str(lock_number), peer, timestamp, timestamp))
            machine_id = cursor.lastrowid
            if machine_id > 99999999:
                raise ValueError("cabinet ID exceeds the ten-character client identifier")
            identifier = f"LC{machine_id:08d}"
            connection.execute("UPDATE cabinet_registry SET identifier=? WHERE id=?",
                               (identifier, machine_id))
        connection.execute(
            "INSERT INTO machines(id,info,registered,last_seen) VALUES(?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET info=excluded.info,last_seen=excluded.last_seen",
            (identifier, payload, timestamp, timestamp))
    return machine_id, identifier


def best_record(previous, result):
    """24-byte native best record, updated as in 0x80da2d3..0x80da54b.

    song/selector/mode are +0/+4/+5, flags +6, grade +7, category +8,
    best score +12 and plays +16. The final word is preserved, not guessed.
    """
    if previous is None:
        record = bytearray(24)
        record[:6] = result[:6]
        record[7:9] = b"\xff\xff"
    else:
        record = bytearray(previous)
    score = struct.unpack_from("<I", result, 12)[0]
    old_score = struct.unpack_from("<I", record, 12)[0]
    if result[7] <= 15:
        if score > old_score or record[7] > 15:
            struct.pack_into("<I", record, 12, score)
            record[7] = result[7]
        record[8] = min(record[8], result[8])
    record[6] |= result[6] & 1
    plays = struct.unpack_from("<I", record, 16)[0]
    struct.pack_into("<I", record, 16, (plays + 1) & 0xffffffff)
    return bytes(record)


def save_best(connection, user_id, result):
    song_id = struct.unpack_from("<I", result)[0]
    key = (user_id, song_id, result[4], result[5])
    previous = connection.execute(
        "SELECT record FROM best_records WHERE user_id=? AND song_id=? AND selector=? AND mode=?",
        key).fetchone()
    raw = best_record(previous[0] if previous else None, result)
    connection.execute(
        "INSERT INTO best_records(user_id,song_id,selector,mode,record,timestamp) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(user_id,song_id,selector,mode) DO UPDATE SET "
        "record=excluded.record,timestamp=excluded.timestamp", key + (raw, now()))


def migrate_results(connection):
    """Recover native records already captured by the earlier login/score server."""
    rows = connection.execute(
        "SELECT s.id,s.user_id,s.judgements FROM scores s LEFT JOIN result_packets r ON r.score_id=s.id "
        "WHERE r.score_id IS NULL ORDER BY s.id").fetchall()
    for score_id, user_id, details in rows:
        fields = json.loads(details or "{}")
        if "raw" not in fields:
            raise ValueError(f"score {score_id} has no native result capture; cannot reconstruct its best record")
        payload = bytes.fromhex(fields["raw"])
        if len(payload) != 108 or struct.unpack_from("<I", payload, 0x1c)[0] != user_id:
            raise ValueError(f"score {score_id} has an invalid native result capture")
        machine_id = struct.unpack_from("<I", payload, 0x18)[0]
        player = struct.unpack_from("<I", payload, 0x60)[0]
        save_best(connection, user_id, payload)
        connection.execute(
            "INSERT INTO result_packets(score_id,machine_id,player,payload) VALUES(?,?,?,?)",
            (score_id, machine_id, player, payload))


def history_pages(db, user_id, player):
    with closing(sqlite3.connect(db)) as connection:
        records = connection.execute(
            "SELECT record FROM best_records WHERE user_id=? ORDER BY song_id,selector,mode LIMIT 10240",
            (user_id,)).fetchall()
    for start in range(0, len(records), 256):
        page = records[start:start + 256]
        yield struct.pack("<4I", player, 0, start, len(page)) + b"".join(row[0] for row in page).ljust(6144, b"\0")


def chart_packet(charts):
    if len(charts) > 32:
        raise ValueError("native chart reward list exceeds 32 entries")
    return struct.pack("<4I", 0, 0, 0, len(charts)) + b"".join(
        struct.pack("<3I", *chart) for chart in charts).ljust(384, b"\0")


def earned_chart_packets(connection, machine_id):
    charts = connection.execute(
        "SELECT DISTINCT r.song_id,r.mode,r.selector FROM event_rewards r "
        "JOIN local_events e ON e.id=r.event_id JOIN event_progress p ON p.event_id=e.id "
        "WHERE p.machine_id=? AND p.plays>=e.target_plays ORDER BY r.song_id,r.mode,r.selector",
        (machine_id,)).fetchall()
    for start in range(0, len(charts), 32):
        yield chart_packet(charts[start:start + 32])


def event_definitions(connection, machine_id):
    for row in connection.execute(
            "SELECT id,word_04,word_08,word_0c,sprite FROM local_events WHERE enabled=1 ORDER BY id"):
        event_id, a, b, c, sprite = row
        charts = connection.execute(
            "SELECT song_id,mode,selector FROM event_charts WHERE event_id=? ORDER BY song_id,mode,selector",
            (event_id,)).fetchall()
        if len(charts) > 32:
            raise ValueError(f"event {event_id} exceeds the native 32-chart list")
        progress = event_percent(connection, event_id, machine_id)
        title = sprite.encode("ascii").ljust(32, b"\0")
        triples = b"".join(struct.pack("<3I", *chart) for chart in charts)
        yield struct.pack("<4If", event_id, a, b, c, progress) + title + struct.pack("<I", len(charts)) + triples.ljust(384, b"\0")


def event_percent(connection, event_id, machine_id):
    row = connection.execute(
        "SELECT e.target_plays,COALESCE(p.plays,0) FROM local_events e "
        "LEFT JOIN event_progress p ON p.event_id=e.id AND p.machine_id=? WHERE e.id=? AND e.enabled=1",
        (machine_id, event_id)).fetchone()
    return min(100.0, 100.0 * row[1] / row[0]) if row else 0.0


def advance_events(connection, machine_id, user_id, result):
    """Local policy: each valid matching chart play advances its configured goal."""
    if result[7] > 15:
        return []
    song_id = struct.unpack_from("<I", result)[0]
    rows = connection.execute(
        "SELECT e.id,e.target_plays,e.title_id FROM local_events e WHERE e.enabled=1 AND "
        "(NOT EXISTS(SELECT 1 FROM event_charts c WHERE c.event_id=e.id) OR "
        "EXISTS(SELECT 1 FROM event_charts c WHERE c.event_id=e.id AND c.song_id=? AND "
        "(c.mode=255 OR (c.mode=? AND c.selector=?))))",
        (song_id, result[5], result[4])).fetchall()
    updated = []
    for event_id, target, title_id in rows:
        connection.execute(
            "INSERT INTO event_progress(event_id,machine_id,plays) VALUES(?,?,1) "
            "ON CONFLICT(event_id,machine_id) DO UPDATE SET plays=plays+1", (event_id, machine_id))
        progress = event_percent(connection, event_id, machine_id)
        updated.append((event_id, progress))
        if user_id and title_id is not None and progress >= 100:
            connection.execute(
                "INSERT OR IGNORE INTO title_rewards(user_id,title_id,event_id,awarded) VALUES(?,?,?,?)",
                (user_id, title_id, event_id, now()))
    return updated


def load_content(db, filename):
    """Optional explicit LAN event/message configuration; absent means none."""
    with open(filename, encoding="utf-8") as source:
        content = json.load(source)
    if len(content.get("events", [])) > 10:
        raise ValueError("native UI supports at most ten event channels")
    with closing(sqlite3.connect(db)) as connection, connection:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("UPDATE local_events SET enabled=0")
        for event in content.get("events", []):
            event_id = int(event["id"])
            charts = event.get("charts", [])
            rewards = event.get("rewards", [])
            sprite = str(event["sprite"])
            if not 0 < len(sprite.encode("ascii")) < 32 or "\0" in sprite:
                raise ValueError("event sprite must be a 1-31 character ASCII basename")
            if len(rewards) > 32:
                raise ValueError("event exceeds native reward count")
            for chart in rewards:
                if len(chart) != 3 or chart[1] not in (0, 64, 128, 192):
                    raise ValueError("reward charts require a concrete native chart mode")
            if len(charts) > 32 or not 0 < event_id <= 0xffffffff:
                raise ValueError("event ID or native chart count out of range")
            words = [int(event.get(f"word_{offset}", 0)) for offset in ("04", "08", "0c")]
            connection.execute(
                "INSERT INTO local_events(id,word_04,word_08,word_0c,sprite,target_plays,title_id,enabled) "
                "VALUES(?,?,?,?,?,?,?,1) ON CONFLICT(id) DO UPDATE SET word_04=excluded.word_04,"
                "word_08=excluded.word_08,word_0c=excluded.word_0c,sprite=excluded.sprite,"
                "target_plays=excluded.target_plays,title_id=excluded.title_id,enabled=1",
                (event_id, *words, sprite, int(event["target_plays"]), event.get("title_id")))
            connection.execute("DELETE FROM event_charts WHERE event_id=?", (event_id,))
            for chart in charts:
                connection.execute("INSERT INTO event_charts VALUES(?,?,?,?)", (event_id, *chart))
            connection.execute("DELETE FROM event_rewards WHERE event_id=?", (event_id,))
            for chart in rewards:
                connection.execute("INSERT INTO event_rewards VALUES(?,?,?,?)", (event_id, *chart))
        connection.execute("UPDATE local_messages SET enabled=0,path=''")
        for slot, message in enumerate(content.get("messages", [])[:2]):
            connection.execute("UPDATE local_messages SET revision=?,enabled=?,path=? WHERE slot=?",
                               (int(message["revision"]), bool(message.get("enabled", True)),
                                str(message.get("path", "")), slot))
