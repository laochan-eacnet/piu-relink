"""Two-cabinet matching and peer relays for the native Phoenix state machine.

Layouts follow builders 0x08083a46..0x08084a90 and reply switch 0x08085a76.
This service pairs actual clients; it never manufactures an opponent or score.
"""
from dataclasses import dataclass
import struct
import threading


@dataclass
class Participant:
    handler: object
    search: bytes
    peer: object = None
    ready: bool = False
    start_requested: bool = False
    started: bool = False
    result: bytes = None
    result_requested: bool = False


class MatchingService:
    COMMANDS = frozenset((0x132019, 0x13201c, 0x13201d, 0x132020,
                          0x132021, 0x132022, 0x132024, 0x132026,
                          0x132027, 0x132028, 0x13202b, 0x13202c))

    def __init__(self):
        self.lock = threading.RLock()
        self.participants = {}
        self.photos = {}

    @staticmethod
    def _size(payload, size):
        if len(payload) != size:
            raise ValueError(f"matching payload length {len(payload)}; expected {size}")

    def _identity(self, handler, payload, size):
        self._size(payload, size)
        if not handler.machine_id or struct.unpack_from("<I", payload)[0] != handler.machine_id:
            raise ValueError("matching identity does not match registered cabinet")

    def _send(self, handler, command, payload=b""):
        try:
            handler.send_packet(command, payload)
            return True
        except OSError as error:
            handler.log(f"matching peer send failed: {error}")
            self._detach(handler)
            return False

    def _detach(self, handler, notify=True):
        participant = self.participants.pop(handler, None)
        if participant is None or participant.peer is None:
            return
        peer = participant.peer
        self.participants.pop(peer.handler, None)
        participant.peer = peer.peer = None
        if notify:
            # 0x132029 resets native matching state and stops its retry timer.
            self._send(peer.handler, 0x132029)

    def disconnected(self, handler):
        with self.lock:
            self._detach(handler)
            self.photos.pop(handler, None)

    def _search(self, handler, payload):
        self._identity(handler, payload, 64)
        previous = self.participants.get(handler)
        if (previous is not None and previous.search == payload and
                not previous.started and previous.result is None):
            self._send(handler, 0x13201a)
            if previous.peer is not None:
                self._send(handler, 0x13201b, previous.peer.search[:48] + payload[48:60])
            return
        self._detach(handler)
        participant = Participant(handler, bytes(payload))
        self.participants[handler] = participant
        if not self._send(handler, 0x13201a):
            return
        # Version, song/mode/chart triple and search mode must agree. FIFO is
        # sufficient for a LAN; clients retain their own display/profile data.
        for other in tuple(self.participants.values()):
            if (other is participant or other.peer is not None or
                    other.handler.machine_id == handler.machine_id):
                continue
            if other.search[4:12] != payload[4:12] or other.search[48:64] != payload[48:64]:
                continue
            participant.peer = other
            other.peer = participant
            if (self._send(other.handler, 0x13201b, payload[:48] + payload[48:60]) and
                    self._send(handler, 0x13201b, other.search[:48] + payload[48:60])):
                self._relay_photo(handler, other.handler)
                self._relay_photo(other.handler, handler)
            handler.log(f"matched cabinets {handler.machine_id}/{other.handler.machine_id}")
            break

    def _start(self, participant):
        peer = participant.peer
        if (peer is not None and participant.start_requested and peer.start_requested and
                participant.ready and peer.ready and not participant.started):
            participant.started = peer.started = True
            self._send(participant.handler, 0x13201f)
            self._send(peer.handler, 0x13201f)

    def _deliver_result(self, participant):
        if (participant.result_requested and participant.peer is not None and
                participant.peer.result is not None):
            self._send(participant.handler, 0x132025, participant.peer.result)

    def _relay_photo(self, sender, recipient):
        chunks = self.photos.get(sender, ())
        if chunks and self._send(recipient, 0x13202a, struct.pack("<I", sender.machine_id)):
            for chunk in chunks:
                if not self._send(recipient, 0x13202e, chunk):
                    break

    def dispatch(self, handler, command, payload):
        if command not in self.COMMANDS:
            return False
        with self.lock:
            if command == 0x132019:
                self._search(handler, payload)
                return True
            if command in (0x132021, 0x13202b, 0x13202c):
                self._size(payload, 4)
            else:
                self._identity(handler, payload, 52 if command == 0x132022 else 16)
            participant = self.participants.get(handler)
            if command in (0x132020, 0x132022, 0x132026, 0x132027, 0x132028, 0x13202c):
                self.photos.pop(handler, None)
            if command in (0x132026, 0x132027, 0x132028, 0x13202c):
                if participant is not None and participant.peer is not None:
                    self.photos.pop(participant.peer.handler, None)
            if command == 0x13202c:
                self._detach(handler)
                # Exit matching after a stage break; capability stays enabled.
                self._send(handler, 0x13202d, struct.pack("<I", 1))
            elif command in (0x132026, 0x132027, 0x132028):
                self._detach(handler)
                reply = {0x132026: 0x132026, 0x132027: 0x132027, 0x132028: 0x132029}[command]
                self._send(handler, reply)
            elif command == 0x13202b:
                # Acknowledges the native screenshot request; no reply loop.
                handler.log("matching screenshot request acknowledged")
            elif participant is None or participant.peer is None:
                handler.log(f"matching command 0x{command:x} without an active opponent")
            elif command == 0x13201c:
                participant.ready = True
                self._start(participant)
            elif command == 0x13201d:
                participant.start_requested = True
                # First acknowledgement moves native 6->7; the shared barrier
                # then moves 7->9, whose handler sends request 0x132020.
                self._send(handler, 0x13201e)
                self._start(participant)
            elif command == 0x132020:
                # Native handler already moved itself to state 10 (playing).
                handler.log("matching gameplay started")
            elif command == 0x132021:
                # Two u16 live combo/display values, NOT participant counts.
                self._send(participant.peer.handler, 0x132021, payload)
            elif command == 0x132022:
                participant.result = bytes(payload[16:52])
                self._send(handler, 0x132023)
                self._deliver_result(participant.peer)
            elif command == 0x132024:
                participant.result_requested = True
                self._deliver_result(participant)
        return True

    def screenshot(self, handler, payload):
        # Native receiver always reads all 8192 chunk bytes, including padding.
        # The caller has already validated/reassembled and persisted the chunk.
        if len(payload) < 12:
            raise ValueError("missing matching screenshot header")
        size, total, index = struct.unpack_from("<3I", payload)
        if not 0 < size <= 8192 or not 0 < total <= 0x14000 or len(payload) < 12 + size:
            raise ValueError("invalid matching screenshot chunk")
        if index * 8192 + size > total:
            raise ValueError("matching screenshot chunk exceeds image")
        with self.lock:
            if index == 0:
                self.photos[handler] = []
            chunks = self.photos.get(handler)
            if chunks is None or index != len(chunks):
                raise ValueError("matching screenshot chunks are not sequential")
            padded = payload[:12 + size].ljust(12 + 8192, b"\0")
            chunks.append(padded)
            participant = self.participants.get(handler)
            if participant is not None and participant.peer is not None:
                recipient = participant.peer.handler
                if index == 0:
                    self._send(recipient, 0x13202a, struct.pack("<I", handler.machine_id))
                self._send(recipient, 0x13202e, padded)
