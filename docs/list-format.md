# Phoenix `D/LIST` catalog format

This document records the format and behavior observed in the supplied `piu` ELF and in the offline `phoenixtools` catalog hook. It deliberately separates proven behavior from names that are only convenient editor labels. The lossless editor is `phoenixtools/tools/list_editor.py`; shared binary parsing is in `phoenixtools/tools/song_catalog_common.py`.

## Files and top-level layout

`D/LIST` is an uncompressed little-endian byte stream:

```text
u32 song_count
u32 chart_count
song_count * 32-byte song records
chart_count * 36-byte chart records
```

Record zero in both arrays is a sentinel/dummy. Native loops generally begin at record 1. The loader at `0x08067774` copies the two counts, allocates the arrays, copies `32 * song_count` bytes followed by `36 * chart_count` bytes, and does not inspect a LIST-specific checksum. The editor preserves any bytes after the expected record payload as `trailing_hex` in JSON.

The loader reads localized resources `D/LIST.0` through `D/LIST.5`. It loads both the selected locale and `LIST.0` (the latter is proven by the assembly around `0x0806785D–0x08067867`). String indices in records are shared across all language tables, so the tables must keep the same entry count and index order.

## Song record (32 bytes)

| Offset | Storage | Editor JSON field | Proven behavior |
|---:|---|---|---|
| `0x00` | `u32` | `id` | Song/resource ID. Used to construct hexadecimal resource paths and persistent chart identity. It is not the array index. (`0x080FE6AC`, `0x080FD792`) |
| `0x04` | `i32` | `title_index` | Localized title string index; negative values produce fallback text. Used by title sorting and display. (`0x080AF862`, `0x080A12A6`) |
| `0x08` | `i32` | `artist_index` | Localized artist/display-credit string index. The display formatter joins this with the BPM text. Calling it “artist” is an editor convenience; the binary does not provide a symbol proving that name. (`0x080B44A2`) |
| `0x0C` | `i32` | `bpm_text_index` | Localized BPM text index. Displayed after `" / BPM "`; playback parses numbers from this text with `strtod`. (`0x080B44A2`, `0x080DBDE4`) |
| `0x10` | `i32` | `fallback_song_index` | Parent/fallback **song array index**, not a string index. Resource lookup walks this when assets are missing and stops on a negative value; the image lookup also uses it. (`0x080FE6AC`, assembly `0x0806D702–0x0806D70A`) |
| `0x14` | `u8` | `byte20` | Series/version category. Channel comparisons group values `1–14`, `15–17`, `18–21`, `22`, `23`, `24`, `25–26`; values above `24` participate in the New Tunes comparison. Exact individual names for every value are not proven. (`0x080A9E78`) |
| `0x15` | `u8` | `byte21` | Genre/channel category. Observed cases: `1` Original, `2` K-pop, `3` World Music, `4` Japan Music, `5` Xross. Values participate in fixed-array channel sorting; arbitrary larger values are unsafe. (`0x080A9E78`, comparator `0x080AF862`) |
| `0x16` | `u8` | `byte22` | Length/category. Observed cases: `1` Shortcut, `2` Normal, `3` Remix, `4` Full. A runtime maximum category is compared against this byte. This is not an unlock level or release number. (`0x080A9E78`, `0x08066ED8`) |
| `0x17` | `u8` | `byte23` | Song flags. Bit `0x80` removes a song from ordinary selection; `(flags & 0x60) == 0x20` independently rejects charts; bit `0x01` includes the song in restriction-menu text generation. Other meanings are unknown. (`0x080A9E78`, `0x08066ED8`, `0x080A12A6`) |
| `0x18–0x1B` | 4 bytes | `bpm_bits` | Unknown/opaque in the investigated consumers. Existing records look like an IEEE-754 `f32` numeric BPM; the editor preserves the raw `u32` and updates it only when `add-song`/`set bpm` receives numeric text. Exact game meaning is not proven. |
| `0x1C` | `i32` | `trailing_index` | Mutable image/resource lookup cache. `-1` triggers `%03X` lookup and fallback; the lookup result overwrites this field. It is not a localized string index despite its position at the end of the record. (`0x0806D6A2`, especially `0x0806D6B6–0x0806D6FA`) |

The `id`, string indices, four opaque bytes, numeric word, and trailing index are all retained by JSON export/import. Removing a song reindexes `fallback_song_index` references; a reference to the removed song becomes `-1`.

## Chart record (36 bytes)

| Offset | Storage | Editor JSON field | Proven behavior |
|---:|---|---|---|
| `0x00` | `i16` | `source_song_index` | Source/resource song array index. It selects the resource ID and defines the contiguous source-chart group used to find the NX basename. Negative values resolve to the sentinel in some consumers. (`0x0806D716`, `0x080FD792`, `0x080DBDE4`) |
| `0x02` | `i16` | `display_song_index` | Display/group song array index. Negative values exclude a chart from selection. Ordinary grouping uses this; the UCS path switches to the source index. (`0x08066ED8`, `0x080FE642`) |
| `0x04` | `i32` | `name_index` | Localized string index containing the NX basename, without `.NX`. Playback constructs the chart path from this value. (`0x0806D716`, `0x080DBDE4`) |
| `0x08` | `u8` | `byte8` | Chart class. Class `1` is required by ordinary random/demo-compatible selection; class `2` is cleared by ordinary channel selection. It is **not** the single/double/co-op layout field. Full class enumeration is unknown. (`0x0806D84A`, `0x080A9E78`, `0x080DBDE4`) |
| `0x09` | `u8` | `level` | Level/selector byte. Ordinary level channels compare it directly; the co-op channel accepts values `>49`. It also participates in persistent chart identity. (`0x080A9E78`, `0x080FD792`) |
| `0x0A` | `u8` | `byte10` | Layout/lock/visibility bitfield. Observed ordinary low values are `0` for single, `2` for double, and `3` for co-op; records also contain `0x20`-high variants (`32`, `34`, `35`) and `0x40`-high variants. The exact meaning of the high bits is not proven. The editor's named chart types alter only the low two bits and preserve the rest. The offline hook sets bit `0x80` for locally backed charts. |
| `0x0B` | `u8` | `byte11` | Unknown; preserve verbatim. |
| `0x0C–0x23` | 24 bytes | `tail_hex` | Unknown/opaque; preserve verbatim. |

Charts for one source song are expected to remain contiguous. The basename search around `0x0806D716` stops when the source index changes, and generated selectable groups use signed 16-bit start/count fields. `add-chart` inserts after the existing contiguous source group; for a newly appended song this is equivalent to appending. Direct JSON edits must preserve the same rule. Other investigated persistent score/best-record structures use song ID plus chart selector (`docs/networking.md` score payload offsets `0` and `4`), not a LIST chart-array index, so shifting chart array indices does not require a score-table migration.

## LIST.N string tables

The decoded LIST.N representation is:

```text
string area
u32 offsets[entry_count]       # offsets from the beginning of string area
u32 footer_distance            # offset-table byte size + 4
```

The offset table begins at `len(file) - footer_distance`. Therefore:

```text
entry_count = (footer_distance - 4) / 4
entry[i] = data[offsets[i] : offsets[i+1]]
entry[last] = data[offsets[last] : offset_table_start]
```

Each entry is NUL-terminated and normally padded to a 4-byte boundary. The loader's equivalent calculation is `offset_table = data + L - u32le(data + L - 4)`, with string offsets read from that table (`0x08067828–0x08067838`, `0x080678A3–0x080678B3`).

The editor stores each entry's decoded text and original raw bytes in JSON. Unchanged entries retain their exact bytes. When text is changed or a new string is appended, the editor rebuilds the string area, offset table, and footer. New title, artist, BPM, and chart-name strings are appended at the same index in every `LIST.0`–`LIST.5` table. A malformed or count-mismatched table is rejected before writing.

## Native visibility and playability gates

Adding a valid record is necessary but not sufficient. The following independent gates were observed.

### Native catalog and channel construction

The native channel rebuild at `0x080A9E78` and selectable-chart construction at `0x08066ED8` apply these checks:

* real chart iteration starts at chart index `1`; chart `0` is a sentinel;
* the display index must be nonnegative;
* both source and display song IDs are tested against the active basic/full restrictions; the source ID is also checked against basic restrictions;
* hardcoded ID `5892` and a substantial legacy-ID blacklist remain independent of LIST edits;
* region state adds restrictions for regions `2` and `4`;
* the runtime maximum category must admit the display song's `byte22` length/category;
* `(song[0x17] & 0x60) == 0x20` rejects a chart;
* ordinary final selection removes songs with `song[0x17] & 0x80`;
* one-player/profile state can reject doubles (`a1 == 2` with the relevant chart flags);
* guest/profile state adds co-op and explicit song-ID restrictions;
* ordinary random/demo-compatible paths require chart class `byte8 == 1` and ordinary channel logic removes class `2`;
* chart groups and source references must stay within signed 16-bit index limits (`0..32767`) and remain contiguous.

The market loader at `0x08071E22` separately reads `market/%s/song.txt` and recognizes `START_SONG`, `SONG_NEW`, `DEMO_SONG`, `RESTRICTED_SONGS_BASIC`, and `RESTRICTED_SONGS_FULL`. Native finalization at `0x080FEC7A` transforms flags as `(old & 0x6F) | 0x10`; persistent entries with selector `0xFF` and a nonzero restriction byte can set song bit `0x80`. Thus an authored flag can be changed after LIST loading.

### Offline phoenixtools policy

`phoenixtools/src/gameplay/catalog.c` runs after the native market finalizer (`catalog.c:189–199`). It does not add selectable-table entries and does not validate NX/media contents.

With `unlock_all_songs` or `guest_full_mode`, song policy requires valid catalog pointers/counts and successful replacement restriction vectors (`catalog.c:55–132`). A song is considered local only when the overlay root or plain root contains the exact directory:

```text
<overlay_root or plain_root>/D/<uppercase hexadecimal song ID>
```

The policy uses `stat()` and requires a directory; it does not use the later case-folding resolver and does not inspect files inside the directory. For local songs it clears bit `0x80` at song offset `0x17`.

With `unlock_all_charts`, chart policy iterates chart indices `1..chart_count-1` (`catalog.c:140–164`). It reads the signed source index at chart `+0x02` in the hook's native struct mapping, requires a nonnegative in-range reference to a nonzero song ID with the exact local directory, and sets chart offset `0x0A` bit `0x80`. The policy preserves chart zero and does not rebuild the selectable table. The hook's source globals and strides are documented in `catalog.c:11–29`.

Therefore an ordinary custom song should have a real nonzero song record, a nonnegative source/display chart reference, and an exact uppercase-ID directory in either configured root. This makes it eligible for the offline unhide hook, but native channel construction, chart parsing, and media loading still have to succeed.

## IDs and resources

There is no universal native “new IDs must be below X” rule established by the investigation. For compatibility, allocate a unique ordinary ID and avoid reserved families observed in native selection/random code:

* `(id & 0xE0000000) == 0xE0000000` — special/basic family;
* `id & 0xFF000 == 0xCF000` — UCS family;
* `id & 0xFF000 == 0xBF000` — online family;
* `id & 0xFF000 == 0xDF000` — synthetic level-channel family;
* `(id & 0xFFFDF000) == 0xDF000`;
* `(id & 0xFFF0F000) == 0xF000`;
* `(id & 0xEF000) == 0xAF000`.

These are editor compatibility rules, not proof of a complete native ID specification. The editor rejects duplicate IDs, zero/sentinel IDs, and IDs in these reserved families for `add-song`.

For a normal song with ID `ID` and an NX basename `STEM`, native playback requires a Phoenix NX20 chart at:

```text
D/<ID>/<ID>/<STEM>.NX
```

The loader at `0x080D033C` rejects a missing file or a magic other than `NX20`; playback path construction is shown around `0x080DCE7A` and `0x080DD070`. The resource resolver first tries the nested directory, then the song directory, then the parent/fallback chain (`0x080FE6AC`). Common assets are:

```text
D/<ID>/A/.A       full audio
D/<ID>/P/A/.A    preview audio
D/<ID>/V/.V2     full video
D/<ID>/P/V/.V2   preview video
D/<ID>/P/.G      preview graphic
D/<ID>/<ID>/LM.NX lightmap/chart support
```

Selection uses preview assets (`0x080B44A2`); gameplay can request full video, preview graphic, and `LM.NX` (`0x080DBDE4`). Missing media can leave a catalog entry visible but can prevent a complete selection/gameplay path. `list_editor.py` validates required folder/NX references and prints missing-media warnings.

No additional per-song LIST hash or checksum was found. Generic packed-resource decoding elsewhere checks decoded length and Adler-32 against the resource-directory entry (`0x08080830`), but that is container integrity, not a new field required for an ordinary decoded LIST edit.

## Song-select channels and custom folders

Song-select channels are not discovered from directories, `CHANNEL.TXT` keys, or arbitrary LIST category values. The native channel table at `0x08218A80–0x08219378` contains 82 fixed records (IDs `0–81`); `0x080AF4CC` walks that table with a `0x1C` stride and builds the selector vectors, while `0x080B4D5A` independently searches the same fixed count. Membership is a hardcoded switch in `0x080A9E78–0x080AB665`, followed by final chart grouping at `0x08066ED8–0x080675BF`. Adding a directory or a `CHANNEL_CUSTOM` localization entry therefore does not add a new channel.

The proven ordinary channel enum is:

| Enum | Channel | Membership predicate (simplified) |
|---:|---|---|
| `0` | New Tunes | display `byte20 > 24`; ordinary path also requires source `byte22 == 2` |
| `1` | K-pop | display `byte21 == 2` |
| `2` | Original | display `byte21 == 1` |
| `3` | World Music | display `byte21 == 3` |
| `4` | Japan Music | display `byte21 == 4` |
| `5` | Xross | display `byte21 == 5` |
| `6` | Shortcut | display `byte22 == 1` |
| `7` | Remix | display `byte22 == 3` |
| `8` | Full | display `byte22 == 4` |
| `9` | Co-op | display `byte22 == 2`, chart level `>49` |
| `10` | Series parent | children `11–17` |
| `11` | 1st–Zero | display or source `byte20` in `1..14` |
| `12` | NX–NXA | display or source `byte20` in `15..17` |
| `13` | Fiesta–Fiesta2 | display or source `byte20` in `18..21` |
| `14` | Prime | display or source `byte20 == 22` |
| `15` | Prime2 | display or source `byte20 == 23` |
| `16` | XX | display or source `byte20 == 24` |
| `17` | Phoenix | display or source `byte20` in `25..26` |
| `18` | Single parent | children `19–42` |
| `43` | Double parent | children `44–69` |
| `70` | UCS | source ID family `(id & 0xFF000) == 0xCF000` |
| `71` | Online | source ID family `(id & 0xFF000) == 0xBF000` |
| `72–81` | Event 01–10 | runtime event eligibility triples |

Single/double level children use the chart `byte10` layout bits and level byte, with special handling for UCS and synthetic `0xDF000` families. The table above is intentionally simplified; the full predicates also apply length, restrictions, flags, profile, region, and lock gates described earlier.

The channel comparator at `0x080AF862–0x080AF963` first ranks `byte21` using only initialized entries `{0:10, 1:4, 2:5, 3:3, 4:2, 5:1}`, then sorts by localized title when the title-sort runtime flag is enabled; otherwise it sorts by descending `byte20` and then ascending song-array index. Do not invent `byte21=6` for Custom: it consumes uninitialized/out-of-range rank data.

Localization and UI artwork are separate from membership:

* `config/lang/%s/channel.txt` is loaded by `0x080BF272` (literal near `0x081D3112`). `data/CONFIG/LANG/EN/CHANNEL.TXT` contains descriptions such as `CHANNEL_NX_TO_NXA`, but no membership definitions.
* Ordinary UI groups are `user_interface/select_song/full_channel1`, `full_channel2`, and `full_channel3` (literals near `0x081D2C38`, `0x081D2C64`, `0x081D2C90`).
* `0x080A1696` uses the native channel record's asset index (`+4`) to select an indexed icon. New `.SPR` files are not enumerated as channels.
* Asset numbering is not the enum: NX–NXA is native enum `12`, but its sidebar sprite is numbered `15` and its voice is `EFF_CH17` (`data/CONFIG/SFX/SELECT.TXT:47`). The main tile is `CHD_SERIES_02_NX.SPR`; the sidebar label is `FA3_CH_15_NX-NXA.SPR`.
* Overlaying a UI group selects one `current_directory` for the whole group (`phoenixtools/src/resources/group_paths.c:67–118`). Copy the complete affected group subtree into an overlay; a partial overlay can shadow the complete plain group without per-file fallback.

`market/%s/song.txt` loaded at `0x08071E22` recognizes only `START_SONG`, `SONG_NEW`, `DEMO_SONG`, and basic/full restriction lists. It has no channel-definition syntax. UCS/Online are not unused ordinary slots: they use reserved ID families and additional runtime state. Event channels `72–81` are the only existing runtime-configured alternative, but they require event eligibility/content and presentation assets; `docs/networking.md:910–931` records their limits and missing UI paths.

### Practical Custom-folder choices

For an offline installation, the repository now provides a runtime hook in `phoenixtools/src/gameplay/custom_channel.c`. Enable it with:

```ini
[catalog]
custom_channel = true
custom_id_min = 0x7F00
custom_id_max = 0x7FFF
```

The hook intercepts the NX–NXA membership body at `0x080AAE7C` (expected nine-byte signature), clears and marks chart `+0x0A` bit `0x80` for charts whose source or display song ID lies in the inclusive range, then resumes native cleanup at `0x080AA3F1`. It does not rewrite song metadata, does not allocate or rebuild selector vectors, and leaves custom songs reachable through other native paths. The default range is vacant in the shipped catalog and avoids the editor's reserved ID masks; it is an ID range, not a song-array range.

This is a **slot replacement**, not an additional enum: enum 12 still supplies the NX–NXA navigation record, tile, sidebar, and voice plumbing. When enabled, the hook replaces that slot's historical NX–NXA membership with the configured Custom range. Event slots are intentionally not used because they require separate event eligibility vectors, loaded icon handles, event navigation, and missing presentation groups. A genuinely new 83rd channel would require broader native selector/table changes.

The hook also replaces the native translation key `CHANNEL_NX_TO_NXA` with `CHANNEL_CUSTOM`. Operators must overlay a complete `CONFIG/LANG/<locale>/CHANNEL.TXT` containing, for example, `CHANNEL_CUSTOM = "Custom songs"`; the hook does not merge or generate translation files. Existing NX–NXA artwork and `EFF_CH17` remain the fallback. For custom artwork/voice, overlay complete affected `full_channel1/2/3` groups and the relevant sound asset; partial group overlays are unsafe.

If official series membership must remain intact, a runtime hook that repurposes a different fixed slot or a full native selector extension is required; changing LIST byte20 alone is not an orthogonal custom tag.

An overlay smoke with metadata-only isolation (before the runtime hook) is `out/channel-newtunes-smoke/snap/frame-3480.png`: it shows `Smoke Custom / Smoke Artist / BPM 100` and `001/001`, proving a channel can contain only the custom song without modifying `data/D`. The runtime hook's dedicated slot-label/artwork smoke should additionally use the `[catalog]` settings and a complete `CHANNEL.TXT` overlay.

## Lossless editor commands

Global options precede the subcommand:

```text
python phoenixtools/tools/list_editor.py --database-root <D> show songs [--id ID]
python phoenixtools/tools/list_editor.py --database-root <D> show charts [--id ID]
python phoenixtools/tools/list_editor.py --database-root <D> export-json --output catalog.json
python phoenixtools/tools/list_editor.py --database-root <D> import-json --input catalog.json
python phoenixtools/tools/list_editor.py --database-root <D> add-song --id ID --title TITLE --artist ARTIST --bpm BPM [--template INDEX]
python phoenixtools/tools/list_editor.py --database-root <D> add-chart --id ID --name STEM --type single|double|co-op --level LEVEL [--flags MASK]
python phoenixtools/tools/list_editor.py --database-root <D> remove-song --id ID
python phoenixtools/tools/list_editor.py --database-root <D> remove-chart --index INDEX
python phoenixtools/tools/list_editor.py --database-root <D> set --song-id ID --field FIELD --value VALUE
python phoenixtools/tools/list_editor.py --database-root <D> set --chart-index INDEX --field FIELD --value VALUE
```

`add-chart` named types change the observed low two bits of chart `+0x0A` while preserving opaque/high bits; co-op requires a raw level of at least `50`, matching the native co-op threshold. Numeric type values are accepted for exact raw editing. `--flags` is an additional mask and is ORed into `byte10`.

Writes stage temporary files, create `<file>.bak` for each changed pre-existing file, and replace each target atomically. Invalid edits are rejected before catalog files are replaced.

## Overlay smoke-test recipe

Do not edit the shipped `data/D` in place. The tested recipe was:

1. Copy `out/game` to a scratch directory.
2. Create `<scratch>/overlay/D`.
3. Clone an existing song folder, for example `data/D/11021` to `<scratch>/overlay/D/7F01`, and rename the nested folder to `7F01`.
4. Copy `data/D/LIST` and `data/D/LIST.0` through `LIST.5` into `<scratch>/overlay/D`.
5. Run `add-song` and `add-chart` against that overlay D directory.
6. In the scratch `phoenixtools.ini`, set:

```ini
[paths]
overlay_root = overlay
plain_root = /piu/data
```

The copied `phoenixtools.ini` maps `/piu/data` in `[mounts]` to the original repository `data` tree, so the overlay shadows only the edited catalog and cloned song. Launch with:

```text
python tools/run.py -t 90 -o game.log -C <scratch> -i find.txt <scratch>/piu.exe
```

The successful test used a script that inserted two credits, entered song select with `S`, and held `C` (the right pad; `Z` is left) until the last entry. The resulting screenshot was `out/list-smoke-run105/snap/frame-3600.png` and visibly showed `Smoke Custom / Smoke Artist / BPM 100` at `121/121`. The modified run logged `songs=941 charts=4969`, `SELECT_SYNC`, and no fatal exception. An unmodified-LIST baseline in `out/list-baseline/game.log` logged `songs=940 charts=4968`, reached `SELECT_SYNC`, and also completed without a fatal exception.

The earlier modified run crashed at `0x0806C0CF` while formatting a string because new indices had been appended after the existing LIST.N offset table. Rebuilding the offset table fixed the crash and is now part of the shared lossless parser.
