# Shipped media audit

Reproduce in WSL: `python3 ffmpeg/audit.py`.

Scanned **14273** data files (64-byte signatures on every file), probed **1349** candidates; **61** probe failures and **0** file-read failures.

ffprobe version 8.0.1-3ubuntu2 Copyright (c) 2007-2025 the FFmpeg developers

## Game input paths

The `.rodata` media literals in piu are `.V2` at `0x081c726d`, `.V` at `0x081c7271`, `.A` at `0x081c7274`, and `D/%s/V` at `0x081d599f`. No ordinary WebM/AVI/MP4/MKV extension literals occur. `phoenixtools/src/resources/vfs_open.c` maps `.V` to `.V2`; `song_paths.c` resolves song/preview video to `V/.V2` and `P/V/.V2`. Both `avformat_open_input` callsites (`0x0806fd86`, `0x08070416`) belong to the video loader and use custom AVIO; the game imports video decoding, not FFmpeg audio decoding.

All **1276 `.V2` files** were probed successfully; no additional video-extension or media-signature candidates were found outside the requested directories. Signature inspection covered all 14273 data files. Extension reconciliation (including dotfile names such as `.V2`) found **0 unprobed video files**.

## Selection

| Selection | Files |
|---|---:|
| `D/*/P/V/**` | 748 |
| `D/*/V/**` | 516 |
| `VIDEO/**` | 85 |

Selection includes every file under VIDEO (including non-media artwork/metadata), both song-video directories, video extensions anywhere, and EBML/Ogg/AVI/MP4/MPEG signatures anywhere. MP3 `.A` files outside VIDEO belong to the separate libmad audio path, not avformat. Full per-file stream metadata, errors, and extension inventory are in `out/linux/ffmpeg/media-audit.json`.

## Container × video codec × audio codec

| Container (ffprobe) | Video codec | Audio codec | Count |
|---|---|---|---:|
| matroska,webm | vp9 | none | 1247 |
| matroska,webm | vp9 | opus | 29 |
| mp3 | none | mp3 | 12 |

Of the WebM videos, song backgrounds contain 479 VP9-only and 29 VP9+Opus files; previews contain 748 VP9-only files; VIDEO contains 20 VP9-only files. All use VP9; none use VP8. The 12 MP3 entries are separate `VIDEO/*/A/.A` audio assets, consumed by libmad rather than avformat. The minimal FFmpeg build enables Matroska/WebM, VP9 and Opus decoders/parsers, `file`, swscale, and swresample (required by the native Opus decoder). It does not enable MP3 or VP8.

The 61 probe failures are **non-video game resources**, not failed/encrypted `.V2` streams: 7 `.BGA` (`BGA3` headers), 39 text `.SPR` sprite descriptions, 14 custom `.G` graphics, and one `LM.NX` (`NX20` chart). They remain listed individually below rather than silently discarded.

## Probe failures

| File | Diagnostic |
|---|---|
| `data/D/1721/V/.BGA` | data/D/1721/V/.BGA: Invalid data found when processing input |
| `data/D/1721/V/1721.SPR` | data/D/1721/V/1721.SPR: Invalid data found when processing input |
| `data/D/1721/V/1721_1_HD.G` | data/D/1721/V/1721_1_HD.G: Invalid data found when processing input |
| `data/D/1721/V/1721_2_HD.G` | data/D/1721/V/1721_2_HD.G: Invalid data found when processing input |
| `data/D/18C7/V/.BGA` | data/D/18C7/V/.BGA: Invalid data found when processing input |
| `data/D/18C7/V/18C7.SPR` | data/D/18C7/V/18C7.SPR: Invalid data found when processing input |
| `data/D/18C7/V/18C7_1_HD.G` | data/D/18C7/V/18C7_1_HD.G: Invalid data found when processing input |
| `data/D/18C7/V/18C7_2_HD.G` | data/D/18C7/V/18C7_2_HD.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/.BGA` | data/VIDEO/COMPANY_LOGO/CN/.BGA: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/CN.G` | data/VIDEO/COMPANY_LOGO/CN/CN.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/CN.SPR` | data/VIDEO/COMPANY_LOGO/CN/CN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_01.G` | data/VIDEO/COMPANY_LOGO/CN/LOGO_01.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_B_A.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_B_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_B_M.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_B_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_IRO.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_IRO.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_MPROJECT.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_MPROJECT.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_NDA.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_NDA.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_SUN.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_SUN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_S_A.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_S_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/CN/LOGO_S_M.SPR` | data/VIDEO/COMPANY_LOGO/CN/LOGO_S_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/.BGA` | data/VIDEO/COMPANY_LOGO/EN/.BGA: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/EN.G` | data/VIDEO/COMPANY_LOGO/EN/EN.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/EN.SPR` | data/VIDEO/COMPANY_LOGO/EN/EN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_01.G` | data/VIDEO/COMPANY_LOGO/EN/LOGO_01.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_B_A.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_B_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_B_M.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_B_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_IRO.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_IRO.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_MPROJECT.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_MPROJECT.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_NDA.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_NDA.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_SUN.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_SUN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_S_A.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_S_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/EN/LOGO_S_M.SPR` | data/VIDEO/COMPANY_LOGO/EN/LOGO_S_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/.BGA` | data/VIDEO/COMPANY_LOGO/KR/.BGA: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/KR.G` | data/VIDEO/COMPANY_LOGO/KR/KR.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/KR.SPR` | data/VIDEO/COMPANY_LOGO/KR/KR.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_01.G` | data/VIDEO/COMPANY_LOGO/KR/LOGO_01.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_B_A.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_B_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_B_M.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_B_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_IRO.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_IRO.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_MPROJECT.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_MPROJECT.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_NDA.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_NDA.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_SUN.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_SUN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_S_A.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_S_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/KR/LOGO_S_M.SPR` | data/VIDEO/COMPANY_LOGO/KR/LOGO_S_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/.BGA` | data/VIDEO/COMPANY_LOGO/SP/.BGA: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_01.G` | data/VIDEO/COMPANY_LOGO/SP/LOGO_01.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_B_A.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_B_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_B_M.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_B_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_IRO.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_IRO.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_MPROJECT.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_MPROJECT.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_NDA.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_NDA.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_SUN.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_SUN.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_S_A.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_S_A.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/LOGO_S_M.SPR` | data/VIDEO/COMPANY_LOGO/SP/LOGO_S_M.SPR: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/SP.G` | data/VIDEO/COMPANY_LOGO/SP/SP.G: Invalid data found when processing input |
| `data/VIDEO/COMPANY_LOGO/SP/SP.SPR` | data/VIDEO/COMPANY_LOGO/SP/SP.SPR: Invalid data found when processing input |
| `data/VIDEO/OP/LM.NX` | data/VIDEO/OP/LM.NX: Invalid data found when processing input |
| `data/VIDEO/UPDATE_INFO/.BGA` | data/VIDEO/UPDATE_INFO/.BGA: Invalid data found when processing input |
| `data/VIDEO/UPDATE_INFO/1020_1_HD.G` | data/VIDEO/UPDATE_INFO/1020_1_HD.G: Invalid data found when processing input |
| `data/VIDEO/UPDATE_INFO/1020_2_HD.G` | data/VIDEO/UPDATE_INFO/1020_2_HD.G: Invalid data found when processing input |
| `data/VIDEO/UPDATE_INFO/INFO.SPR` | data/VIDEO/UPDATE_INFO/INFO.SPR: Invalid data found when processing input |

## Build and ABI evidence

### Source selection and bundled versions

`ffmpeg/build.sh` pins **FFmpeg 4.2.11**, the latest 4.2 point release in the upstream release index, and checks the archive SHA-256 before extraction. Source archives and extracted trees live in `ffmpeg/`. The baseline 4.2 archive is retained for the independent public-header layout comparison.

```text
d53f0ec8ebb8b03a9ca1efcb93f38ba3f1137b3dcc4ad3239809d511553c97f8  ffmpeg-4.2.11.tar.xz
023f10831a97ad93d798f53a3640e55cd564abfeba807ecbe8524dac4fedecd5  ffmpeg-4.2.tar.xz
```

The bundled filenames were checked against `DT_SONAME` with `readelf -d` and the actual immediate returned by each exported `*_version()` function with `i686-linux-gnu-objdump -d --disassemble=NAME_version`:

| Bundled file in `lib/` | SONAME | Exported version |
|---|---|---|
| `libavcodec.so.58.54.100` | `libavcodec.so.58` | 58.54.100 |
| `libavformat.so.58.29.100` | `libavformat.so.58` | 58.29.100 |
| `libavutil.so.56.31.100` | `libavutil.so.56` | 56.31.100 |
| `libswscale.so.5.5.100` | `libswscale.so.5` | 5.5.100 |
| `libswresample.so.3.5.100` | `libswresample.so.3` | 3.5.100 |

Both upstream 4.2 and 4.2.11 have these exact library micro versions. These numbers establish the 4.2 ABI, but do not distinguish which security point release produced the originals. Using latest 4.2.11 therefore matches the bundled ABI without claiming an unknowable original point-release number.

### Public i386 struct layout

Compiled both probes with the existing Windows-shell `LNX_CC` from `tools/env.sh`:

```bash
. tools/env.sh
"${LNX_CC[@]}" -Wno-deprecated-declarations -I ffmpeg/build -I ffmpeg/ffmpeg-4.2 ffmpeg/layout_test.c -o out/linux/ffmpeg/layout-4.2
"${LNX_CC[@]}" -Wno-deprecated-declarations -I ffmpeg/build -I ffmpeg/ffmpeg-4.2.11 ffmpeg/layout_test.c -o out/linux/ffmpeg/layout-4.2.11
```

Both compiler commands exited 0 with no output. Ran each under WSL:

```bash
LD_LIBRARY_PATH=/mnt/d/tmp/piurelink/out/linux/ffmpeg:/mnt/d/tmp/piurelink/sysroot/lib/i386-linux-gnu \
qemu-i386 -L /mnt/d/tmp/piurelink/sysroot /mnt/d/tmp/piurelink/out/linux/ffmpeg/layout-4.2
LD_LIBRARY_PATH=/mnt/d/tmp/piurelink/out/linux/ffmpeg:/mnt/d/tmp/piurelink/sysroot/lib/i386-linux-gnu \
qemu-i386 -L /mnt/d/tmp/piurelink/sysroot /mnt/d/tmp/piurelink/out/linux/ffmpeg/layout-4.2.11
```

Both exited 0, and a byte-for-byte Python comparison of stdout passed. Full identical outputs are `out/linux/ffmpeg/layout-4.2.txt` and `layout-4.2.11.txt`. Selected output:

```text
headers: avcodec=58.54.100 avformat=58.29.100 avutil=56.31.100 swscale=5.5.100
sizeof(AVPacket)=68
AVPacket.buf=0
AVPacket.pts=4
AVPacket.dts=12
AVPacket.data=20
AVPacket.size=24
AVPacket.stream_index=28
AVPacket.flags=32
AVPacket.side_data=36
AVPacket.side_data_elems=40
AVPacket.duration=44
AVPacket.pos=52
AVPacket.convergence_duration=60
AVFormatContext.pb=16
AVFormatContext.streams=28
AVFormatContext.flags=1092
AVStream.r_frame_rate=156
AVStream.codecpar=168
AVCodecParameters.codec_id=4
AVCodecContext.width=88
AVCodecContext.height=92
AVCodecContext.pix_fmt=108
AVFrame.data=0
AVFrame.linesize=32
AVFrame.height=72
```

The probe has compile-time assertions for the game-observed offsets: packet data/size/index accesses at `0x080ed6aa`–`0x080ed6d4`; format streams/flags, stream codecpar/r_frame_rate and codec width/height in the loaders at `0x0806be79` and `0x0806fd57`–`0x0806fe09`; frame linesize/height and codec pix_fmt in the decode/scale loop at `0x080ed72a`–`0x080ed84f`. It additionally compares AVPicture, AVProbeData and AVIOContext layouts.

The explicit focal libc path in `LD_LIBRARY_PATH` is necessary on this WSL installation: using only `qemu-i386 -L sysroot` let the host loader cache select the host i386 libc, producing a GLIBC_2.35 loader-version error. No sysroot files were changed to hide that mismatch.

### Build invocation and relocation policy

```bash
wsl -e bash /mnt/d/tmp/piurelink/ffmpeg/build.sh
```

The build uses Clang's `i386-linux-gnu` target with the focal sysroot/GCC 10 support files, shared libraries and GNU symbol versioning, pthreads, no stack protector, no autodetected external libraries, no networking/iconv, and only the audited codec/container components. `-static-libgcc` prevents a libgcc_s dependency. All exported SONAME, fully versioned, and unversioned `.so` copies are ordinary files, not symlinks. `build.sh` runs `ffmpeg/verify.py` before reporting success.

`--disable-asm` is intentional, not an untested optimization choice. With `--enable-pic`, the NASM build was rejected by `-Wl,-z,text` for `R_386_32` against a local constant in `libavutil/x86/float_dsp.o` (`.text+0x795`). Disabling only NASM exposed the same problem in swscale's inline-MMX `rgb2rgb_template.c:834`. The final configuration disables all hand-written assembly and keeps the portable C implementations; `-z text` remains enabled. Real-time Prism gameplay performance has not been measured.

The first GLIBC symbol-version check caught `fmod@GLIBC_2.43` and `hypot@GLIBC_2.35` in avutil despite `--sysroot`. Linker tracing showed that the sysroot's developer `libm.so` had been dereferenced against WSL's host absolute symlink target during sysroot generation. The integrator repaired `tools/mksysroot.sh` to re-root absolute `.deb` symlinks and replaced contaminated developer-library copies with focal files. All five FFmpeg shared libraries were then forced to relink against that repaired sysroot. No symbol-version rewriting or compatibility shims were used.

### Final ELF checks

The final build command exited **0** after relinking against the repaired focal sysroot. Its output files and sizes were:

```text
libavcodec.so.58.54.100 (4170588 bytes; SONAME libavcodec.so.58)
libavformat.so.58.29.100 (1234836 bytes; SONAME libavformat.so.58)
libavutil.so.56.31.100 (1663208 bytes; SONAME libavutil.so.56)
libswscale.so.5.5.100 (1408436 bytes; SONAME libswscale.so.5)
libswresample.so.3.5.100 (221740 bytes; SONAME libswresample.so.3)
```

The build's final checker is also directly reproducible:

```bash
wsl -e python3 /mnt/d/tmp/piurelink/ffmpeg/verify.py
```

It runs `readelf -h`, `-d`, `-rW`, `-lW`, `--dyn-syms --wide`, and `-V`, plus `i686-linux-gnu-objdump -d` and `i686-linux-gnu-nm -D --defined-only`. It rejects every `%gs` instruction reference, TLS relocation/segment, TEXTREL, post-2.31 GLIBC requirement, unexpected DT_NEEDED entry, wrong SONAME/version namespace, missing piu import, nonidentical alias, or symlink. The successful build invocation quoted this output (also saved verbatim in `out/linux/ffmpeg/verification.txt`):

```text
libavcodec.so.58: ELF32 i386; SONAME=libavcodec.so.58; symbol_version=LIBAVCODEC_58; max_glibc=GLIBC_2.29
  NEEDED=libswresample.so.3,libavutil.so.56,libm.so.6,libpthread.so.0,libc.so.6
  gs_references=0 TLS_relocations=0 PT_TLS=0 TEXTREL=0 aliases=3 real identical files
libavformat.so.58: ELF32 i386; SONAME=libavformat.so.58; symbol_version=LIBAVFORMAT_58; max_glibc=GLIBC_2.7
  NEEDED=libavcodec.so.58,libavutil.so.56,libm.so.6,libpthread.so.0,libc.so.6
  gs_references=0 TLS_relocations=0 PT_TLS=0 TEXTREL=0 aliases=3 real identical files
libavutil.so.56: ELF32 i386; SONAME=libavutil.so.56; symbol_version=LIBAVUTIL_56; max_glibc=GLIBC_2.29
  NEEDED=libm.so.6,libpthread.so.0,libc.so.6
  gs_references=0 TLS_relocations=0 PT_TLS=0 TEXTREL=0 aliases=3 real identical files
libswscale.so.5: ELF32 i386; SONAME=libswscale.so.5; symbol_version=LIBSWSCALE_5; max_glibc=GLIBC_2.29
  NEEDED=libavutil.so.56,libm.so.6,libc.so.6
  gs_references=0 TLS_relocations=0 PT_TLS=0 TEXTREL=0 aliases=3 real identical files
libswresample.so.3: ELF32 i386; SONAME=libswresample.so.3; symbol_version=LIBSWRESAMPLE_3; max_glibc=GLIBC_2.29
  NEEDED=libavutil.so.56,libm.so.6,libc.so.6
  gs_references=0 TLS_relocations=0 PT_TLS=0 TEXTREL=0 aliases=3 real identical files
piu FFmpeg imports: 33/33 defined at the exact requested symbol version; missing=0
  av_dict_set@LIBAVUTIL_56
  av_find_best_stream@LIBAVFORMAT_58
  av_frame_alloc@LIBAVUTIL_56
  av_frame_free@LIBAVUTIL_56
  av_frame_unref@LIBAVUTIL_56
  av_free@LIBAVUTIL_56
  av_freep@LIBAVUTIL_56
  av_get_media_type_string@LIBAVUTIL_56
  av_init_packet@LIBAVCODEC_58
  av_malloc@LIBAVUTIL_56
  av_packet_unref@LIBAVCODEC_58
  av_probe_input_format@LIBAVFORMAT_58
  av_read_frame@LIBAVFORMAT_58
  av_seek_frame@LIBAVFORMAT_58
  av_strerror@LIBAVUTIL_56
  avcodec_alloc_context3@LIBAVCODEC_58
  avcodec_decode_video2@LIBAVCODEC_58
  avcodec_find_decoder@LIBAVCODEC_58
  avcodec_flush_buffers@LIBAVCODEC_58
  avcodec_free_context@LIBAVCODEC_58
  avcodec_open2@LIBAVCODEC_58
  avcodec_parameters_to_context@LIBAVCODEC_58
  avformat_alloc_context@LIBAVFORMAT_58
  avformat_close_input@LIBAVFORMAT_58
  avformat_find_stream_info@LIBAVFORMAT_58
  avformat_open_input@LIBAVFORMAT_58
  avio_alloc_context@LIBAVFORMAT_58
  avio_context_free@LIBAVFORMAT_58
  avpicture_fill@LIBAVCODEC_58
  avpicture_get_size@LIBAVCODEC_58
  sws_freeContext@LIBSWSCALE_5
  sws_getContext@LIBSWSCALE_5
  sws_scale@LIBSWSCALE_5
PASS: all rebuilt libraries meet the relinker ELF constraints
```

### Real media decode

Compiled `ffmpeg/decode_test.c` with the existing `LNX_CC` (command exited 0 with no output):

```bash
. tools/env.sh
"${LNX_CC[@]}" -Wno-deprecated-declarations -I ffmpeg/build -I ffmpeg/ffmpeg-4.2.11 ffmpeg/decode_test.c -L out/linux/ffmpeg -lavformat -lavcodec -lswscale -lavutil -o out/linux/ffmpeg/decode-test
```

The test uses caller-owned `AVPacket`, the legacy `avcodec_decode_video2` API, `avpicture_fill`, and swscale to RGB24, matching the game's ABI-sensitive path. Ran these commands in WSL:

```bash
LD_LIBRARY_PATH=/mnt/d/tmp/piurelink/out/linux/ffmpeg:/mnt/d/tmp/piurelink/sysroot/lib/i386-linux-gnu \
qemu-i386 -L /mnt/d/tmp/piurelink/sysroot /mnt/d/tmp/piurelink/out/linux/ffmpeg/decode-test /mnt/d/tmp/piurelink/data/VIDEO/STAGEBREAK/V/.V2 30
LD_LIBRARY_PATH=/mnt/d/tmp/piurelink/out/linux/ffmpeg:/mnt/d/tmp/piurelink/sysroot/lib/i386-linux-gnu \
qemu-i386 -L /mnt/d/tmp/piurelink/sysroot /mnt/d/tmp/piurelink/out/linux/ffmpeg/decode-test /mnt/d/tmp/piurelink/data/D/1152/V/.V2 30
```

Both exited **0**. The first input is VP9-only; the second has VP9+Opus streams. Observed stdout/stderr, retained in `out/linux/ffmpeg/decode-results.txt`:

```text
data/VIDEO/STAGEBREAK/V/.V2 exit 0
decoded_frames=30 dimensions=1280x720 output=RGB24 checksum=e4bb1414 codec=vp9
[swscaler @ 0x41fb00] No accelerated colorspace conversion found from yuv420p to rgb24.
data/D/1152/V/.V2 exit 0
decoded_frames=30 dimensions=1280x720 output=RGB24 checksum=39511dc5 codec=vp9
[swscaler @ 0x447120] No accelerated colorspace conversion found from yuv420p to rgb24.
```

The swscale diagnostics are the expected portable-C path, not decode failures. These tests establish real video decoding and RGB conversion, not real-time Windows gameplay performance or a full decode of every asset.

