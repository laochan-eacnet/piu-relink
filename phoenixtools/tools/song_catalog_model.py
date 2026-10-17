"""Lossless binary LIST model shared by catalog tools."""
from __future__ import annotations
import os, struct, tempfile
from dataclasses import dataclass
from pathlib import Path

SONG_RECORD_SIZE=32; CHART_RECORD_SIZE=36; INVALID_INDEX=-1
SONG_FIELDS=(("id","I"),("title_index","i"),("artist_index","i"),("bpm_text_index","i"),("fallback_song_index","i"),("byte20","B"),("byte21","B"),("byte22","B"),("byte23","B"),("bpm_bits","I"),("trailing_index","i"))
CHART_FIELDS=(("source_song_index","h"),("display_song_index","h"),("name_index","i"),("byte8","B"),("level","B"),("byte10","B"),("byte11","B"))
SONG_STRUCT=struct.Struct("<"+"".join(f for _,f in SONG_FIELDS)); CHART_PREFIX=struct.Struct("<"+"".join(f for _,f in CHART_FIELDS))

def _hex(value: object, label: str)->bytes:
    if not isinstance(value,str): raise ValueError(f"{label} must be a hexadecimal string")
    try: return bytes.fromhex(value)
    except ValueError as e: raise ValueError(f"{label} is not valid hexadecimal") from e

def _pack(record: dict, fields: tuple, layout: struct.Struct, label: str, tail: int=0)->bytes:
    wanted={n for n,_ in fields}|({"tail_hex"} if tail else set())
    if not isinstance(record,dict) or set(record)!=wanted: raise ValueError(f"{label} requires exactly these fields: {', '.join(sorted(wanted))}")
    try: data=layout.pack(*(record[n] for n,_ in fields))
    except (struct.error,TypeError) as e: raise ValueError(f"{label}: invalid scalar field") from e
    if any(not isinstance(record[n],int) or isinstance(record[n],bool) for n,_ in fields): raise ValueError(f"{label} scalar fields must be integers")
    if tail:
        raw=_hex(record["tail_hex"],f"{label}.tail_hex")
        if len(raw)!=tail: raise ValueError(f"{label}.tail_hex must contain {tail} bytes")
        data+=raw
    return data

def _stage(target: Path, data: bytes)->Path:
    fd,name=tempfile.mkstemp(prefix=target.name+".",suffix=".tmp",dir=target.parent); path=Path(name)
    try:
        with os.fdopen(fd,"wb") as stream: stream.write(data); stream.flush(); os.fsync(stream.fileno())
    except BaseException: path.unlink(missing_ok=True); raise
    return path

@dataclass
class StringEntry:
    text: str
    raw: bytes
    def encoded(self)->bytes:
        if not isinstance(self.text,str) or "\0" in self.text: raise ValueError("string text must be text without NUL")
        if self.raw and self.text==self.raw.split(b"\0",1)[0].decode("utf-8",errors="replace"): return self.raw
        data=self.text.encode()+b"\0"; return data+b"\0"*((-len(data))%4)

@dataclass
class StringTable:
    entries: list[StringEntry]
    @classmethod
    def parse(cls,data:bytes)->"StringTable":
        if len(data)<4: raise ValueError("LIST.N string table is shorter than its footer")
        distance=struct.unpack_from("<I",data,len(data)-4)[0]; start=len(data)-distance
        if distance<8 or not 0<=start<=len(data)-4 or (distance-4)%4: raise ValueError("LIST.N has an invalid offset-table footer")
        count=(distance-4)//4; entries=[]; previous=0
        for i in range(count):
            begin=struct.unpack_from("<I",data,start+4*i)[0]; end=struct.unpack_from("<I",data,start+4*(i+1))[0] if i+1<count else start
            if begin<previous or end<begin or end>start or b"\0" not in data[begin:end]: raise ValueError(f"LIST.N string offset {i} is invalid")
            raw=data[begin:end]; entries.append(StringEntry(raw.split(b"\0",1)[0].decode("utf-8",errors="replace"),raw)); previous=begin
        return cls(entries)
    def encoded(self)->bytes:
        area=b""; offsets=[]
        for entry in self.entries: offsets.append(len(area)); area+=entry.encoded()
        table=b"".join(struct.pack("<I",o) for o in offsets)
        return area+table+struct.pack("<I",len(table)+4)
    def to_json(self): return [{"text":e.text,"raw_hex":e.raw.hex()} for e in self.entries]
    @classmethod
    def from_json(cls, values, name):
        if not isinstance(values,list): raise ValueError(f"{name} must be an array")
        out=[]
        for i,value in enumerate(values):
            if not isinstance(value,dict) or set(value)!={"text","raw_hex"}: raise ValueError(f"{name}[{i}] requires text and raw_hex")
            text=value["text"]; raw=_hex(value["raw_hex"],f"{name}[{i}].raw_hex")
            if not isinstance(text,str) or "\0" in text: raise ValueError(f"{name}[{i}].text must be text without NUL")
            out.append(StringEntry(text,raw))
        return cls(out)

@dataclass
class Catalog:
    database: Path
    songs: list[dict]
    charts: list[dict]
    tables: dict[str,StringTable]
    trailing: bytes=b""
    @classmethod
    def load(cls,database:Path)->"Catalog":
        database=Path(database); raw=(database/"LIST").read_bytes()
        if len(raw)<8: raise ValueError("LIST is shorter than its eight-byte header")
        songs_n,charts_n=struct.unpack_from("<II",raw); expected=8+songs_n*32+charts_n*36
        if len(raw)<expected: raise ValueError(f"truncated LIST: {len(raw)} bytes, expected {expected}")
        songs=[dict(zip((n for n,_ in SONG_FIELDS),SONG_STRUCT.unpack_from(raw,8+32*i))) for i in range(songs_n)]
        base=8+32*songs_n; charts=[]
        for i in range(charts_n):
            off=base+36*i; rec=dict(zip((n for n,_ in CHART_FIELDS),CHART_PREFIX.unpack_from(raw,off))); rec["tail_hex"]=raw[off+12:off+36].hex(); charts.append(rec)
        tables={p.name:StringTable.parse(p.read_bytes()) for p in sorted(database.glob("LIST.*")) if p.is_file() and p.name[5:].isascii() and p.name[5:].isdigit()}
        if not tables: raise ValueError(f"no LIST.N string tables found in {database}")
        return cls(database,songs,charts,tables,raw[expected:])
    def text(self,index:int,language:int=0)->str:
        table=self.tables.get(f"LIST.{language}"); return table.entries[index].text if table and 0<=index<len(table.entries) else ""
    def append_string(self,text:str)->int:
        sizes={len(t.entries) for t in self.tables.values()}
        if len(sizes)!=1 or any(f"LIST.{i}" not in self.tables for i in range(6)): raise ValueError("LIST.N tables must be synchronized")
        if not isinstance(text,str) or "\0" in text: raise ValueError("new strings must be text without NUL")
        index=next(iter(sizes))
        for table in self.tables.values(): table.entries.append(StringEntry(text,b""))
        return index
    def to_json(self): return {"format":"phoenix-list-v1","songs":self.songs,"charts":self.charts,"string_tables":{n:t.to_json() for n,t in self.tables.items()},"trailing_hex":self.trailing.hex()}
    @classmethod
    def from_json(cls,payload,database):
        required={"format","songs","charts","string_tables","trailing_hex"}
        if not isinstance(payload,dict) or set(payload)!=required or payload["format"]!="phoenix-list-v1": raise ValueError("invalid catalog JSON")
        tables={n:StringTable.from_json(v,n) for n,v in payload["string_tables"].items() if isinstance(n,str) and n.startswith("LIST.") and n[5:].isascii() and n[5:].isdigit()}
        if len(tables)!=len(payload["string_tables"]): raise ValueError("invalid string table filename")
        catalog=cls(Path(database),payload["songs"],payload["charts"],tables,_hex(payload["trailing_hex"],"trailing_hex")); catalog.encoded_files(); return catalog
    def encoded_files(self):
        if not self.songs or not self.charts: raise ValueError("LIST requires dummy records")
        if len(self.songs)>32768: raise ValueError("song indices exceed signed 16-bit chart references")
        songs=b"".join(_pack(s,SONG_FIELDS,SONG_STRUCT,f"songs[{i}]") for i,s in enumerate(self.songs)); charts=b"".join(_pack(c,CHART_FIELDS,CHART_PREFIX,f"charts[{i}]",24) for i,c in enumerate(self.charts))
        for i,c in enumerate(self.charts[1:],1):
            for field in ("source_song_index","display_song_index"):
                if not 0<=c[field]<len(self.songs): raise ValueError(f"charts[{i}].{field} is out of range")
        minimum=min(len(t.entries) for t in self.tables.values())
        for i,s in enumerate(self.songs):
            for field in ("title_index","artist_index","bpm_text_index","trailing_index"):
                if not -1<=s[field]<minimum: raise ValueError(f"songs[{i}].{field} is absent from a string table")
        for i,c in enumerate(self.charts):
            if not -1<=c["name_index"]<minimum: raise ValueError(f"charts[{i}].name_index is absent from a string table")
        result={n:t.encoded() for n,t in self.tables.items()}; result["LIST"]=struct.pack("<II",len(self.songs),len(self.charts))+songs+charts+self.trailing; return result
    def validate(self): self.encoded_files()
    def write(self):
        payloads=self.encoded_files(); self.database.mkdir(parents=True,exist_ok=True); originals={}; staged={}; replaced=[]; backups=[]
        try:
            for name,data in payloads.items():
                target=self.database/name; old=target.read_bytes() if target.exists() else None
                if old!=data: originals[target]=old; staged[target]=_stage(target,data)
            for target,old in originals.items():
                if old is not None:
                    temp=_stage(target.with_name(target.name+".bak"),old); backups.append(temp); os.replace(temp,target.with_name(target.name+".bak"))
            for target,temp in staged.items(): os.replace(temp,target); replaced.append(target)
        except BaseException:
            for target in reversed(replaced):
                old=originals[target]
                if old is None: target.unlink(missing_ok=True)
                else: temp=_stage(target,old); os.replace(temp,target)
            raise
        finally:
            for temp in (*staged.values(),*backups): temp.unlink(missing_ok=True)
        return replaced
