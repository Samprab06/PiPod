#!/usr/bin/env python3
"""Generate test_media/ for the media metadata parser (host tests + simulator).

Creates short (3 s) synthetic sine/chord clips with fictional tags in every
container the firmware supports:

  * MP3  ID3v2.3 + ID3v1 + APIC (ffmpeg), ID3v2.4 UTF-8 VBR (ffmpeg) + footer,
         hand-written ID3v2.3 UTF-16 + extended header, ID3v2.2 (PIC),
         whole-tag unsynchronisation with 0xFF-laden JPEG art, ID3v1 only,
         no tags at all;
  * FLAC Vorbis comments + PICTURE (JPEG and PNG);
  * WAV  LIST/INFO and a trailing `id3 ` chunk;
  * Ogg Vorbis with METADATA_BLOCK_PICTURE (base64 JPEG) and Ogg Opus.

ffmpeg cannot express several of the edge cases (ID3 unsync, UTF-16 frames,
ID3v2.2, raw LIST INFO is fine but the `id3 ` chunk is easier by hand), so the
tag/container bytes for those files are written directly from Python.  The
audio payload always comes from ffmpeg.

Outputs (not committed):
  test_media/*.mp3|flac|wav|ogg|opus   the clips
  test_media/covers/*                  cover art assets (JPEG/PNG)
  test_media/expected.json             authoritative tags + ffprobe data + layout
  test_media/expected.tsv              the same, flat, for the C host test

Run:  python -I scripts/gen_test_media.py
Needs: ffmpeg + ffprobe on PATH, Pillow (PIL).
"""

from __future__ import annotations

import base64
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "test_media"
COVDIR = OUT / "covers"
FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")

CLIP_SECONDS = 3.0
RATE = 44100


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("command failed: %s\n%s" % (
            " ".join(cmd), proc.stderr.decode("utf-8", "replace")))


def be32(n: int) -> bytes:
    return struct.pack(">I", n)


def le32(n: int) -> bytes:
    return struct.pack("<I", n)


def syncsafe(n: int) -> bytes:
    if n >= (1 << 28):
        raise ValueError("syncsafe overflow")
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def unsync(data: bytes) -> bytes:
    """ID3v2 full unsynchronisation: insert 0x00 after every 0xFF."""
    out = bytearray()
    for b in data:
        out.append(b)
        if b == 0xFF:
            out.append(0)
    return bytes(out)


def utf8_trunc(text: str, field_size: int) -> str:
    """What media_meta_t stores: UTF-8, at most field_size-1 bytes, char boundary."""
    raw = text.encode("utf-8")
    if len(raw) < field_size:
        return text
    raw = raw[: field_size - 1]
    while raw:
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raw = raw[:-1]
    return ""


def ffprobe(path: Path) -> dict:
    proc = subprocess.run(
        [
            FFPROBE, "-v", "error", "-select_streams", "a:0",
            "-show_entries", "stream=codec_name,sample_rate,channels,bits_per_raw_sample,bits_per_sample",
            "-show_entries", "format=duration,bit_rate",
            "-of", "json", str(path),
        ],
        check=True, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    info = json.loads(proc.stdout)
    st = (info.get("streams") or [{}])[0]
    fm = info["format"]
    return {
        "duration_ms": int(round(float(fm["duration"]) * 1000)),
        "ffprobe_bit_rate": int(fm.get("bit_rate") or 0),
        "sample_rate": int(st.get("sample_rate") or 0),
        "channels": int(st.get("channels") or 0),
        "bits": int(st.get("bits_per_raw_sample") or st.get("bits_per_sample") or 0),
        "codec": st.get("codec_name") or "",
    }


def ffprobe_tags(path: Path) -> dict:
    proc = subprocess.run(
        [FFPROBE, "-v", "error", "-show_entries", "format_tags:stream_tags",
         "-of", "json", str(path)],
        check=True, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    doc = json.loads(proc.stdout)
    tags = dict(doc.get("format", {}).get("tags", {}))
    for stream in doc.get("streams", []):
        tags.update(stream.get("tags", {}))
    return {k.lower(): v for k, v in tags.items()}


# --------------------------------------------------------------------------- #
# cover art
# --------------------------------------------------------------------------- #
def make_covers(tmp: Path) -> dict:
    COVDIR.mkdir(parents=True, exist_ok=True)
    specs = {
        "cover_300.jpg": ("art_0.png", (300, 300), 78),
        "cover_500.jpg": ("art_1.png", (500, 500), 72),
        "cover_400x300.jpg": ("art_2.png", (400, 300), 78),
        "cover_300.png": ("art_3.png", (300, 300), None),
    }
    out = {}
    for name, (src, size, quality) in specs.items():
        img = Image.open(ROOT / "assets" / "demo_art" / src).convert("RGB")
        img = img.resize(size, Image.LANCZOS)
        path = COVDIR / name
        if quality is None:
            img.save(path, "PNG")
        else:
            img.save(path, "JPEG", quality=quality, optimize=False)
        out[name] = {
            "path": path,
            "bytes": path.read_bytes(),
            "mime": "image/png" if name.endswith(".png") else "image/jpeg",
            "type": "png" if name.endswith(".png") else "jpeg",
            "width": size[0],
            "height": size[1],
        }
    return out


# --------------------------------------------------------------------------- #
# ID3v2 / ID3v1 writers
# --------------------------------------------------------------------------- #
def id3_text(enc: int, text: str) -> bytes:
    if enc == 0:
        return bytes([0]) + text.encode("latin-1")
    if enc == 1:  # UTF-16 with BOM, little endian
        return bytes([1]) + b"\xff\xfe" + text.encode("utf-16-le")
    if enc == 3:
        return bytes([3]) + text.encode("utf-8")
    raise ValueError(enc)


def apic_payload(cover: dict, enc: int = 0, desc: str = "", pic_type: int = 3) -> tuple[bytes, int]:
    """APIC payload plus the byte offset of the picture data inside it."""
    head = bytes([enc]) + cover["mime"].encode("ascii") + b"\x00" + bytes([pic_type])
    head += desc.encode("latin-1") + b"\x00"
    return head + cover["bytes"], len(head)


def pic_payload(cover: dict, enc: int = 0, desc: str = "", pic_type: int = 3) -> tuple[bytes, int]:
    """ID3v2.2 PIC payload: <enc> <3-byte format> <type> <desc NUL> <data>."""
    fmt = b"PNG" if cover["type"] == "png" else b"JPG"
    head = bytes([enc]) + fmt + bytes([pic_type]) + desc.encode("latin-1") + b"\x00"
    return head + cover["bytes"], len(head)


def build_id3v2(
    version: int,
    frames: list[tuple[bytes, bytes, bytes]],  # (id, payload, extra header bytes)
    *,
    tag_unsync: bool = False,
    extended_header: bytes = b"",
    footer: bool = False,
    art_payload_off: int | None = None,  # payload offset (inside the frame) of the picture
    art_frame_index: int | None = None,
    padding: int = 32,
) -> tuple[bytes, int | None, int | None]:
    """Assemble a tag. Returns (tag_bytes, art_file_offset, art_encoded_size).

    The picture offset is tracked through the optional whole-tag unsync so the
    caller gets the exact file offset/size of the encoded picture bytes.
    """
    body = bytearray(extended_header)
    art_body_off = None
    for idx, (fid, payload, extra) in enumerate(frames):
        if version == 2:
            body += fid + len(payload).to_bytes(3, "big") + payload
        elif version == 3:
            body += fid + be32(len(payload)) + extra + payload
        elif version == 4:
            body += fid + syncsafe(len(payload)) + extra + payload
        else:
            raise ValueError(version)
        if art_frame_index is not None and idx == art_frame_index:
            hdr = 6 if version == 2 else 10
            art_body_off = len(body) - len(payload) + art_payload_off
    body += b"\x00" * padding

    if art_body_off is not None and art_frame_index is not None:
        payload = frames[art_frame_index][1]
        pic = payload[art_payload_off:]
        enc_size = len(unsync(pic)) if tag_unsync else len(pic)
        if tag_unsync:
            art_body_off = len(unsync(bytes(body[:art_body_off])))
    else:
        enc_size = None

    stored = unsync(bytes(body)) if tag_unsync else bytes(body)
    flags = 0x80 if tag_unsync else 0
    if extended_header and version >= 3:
        flags |= 0x40
    if footer:
        flags |= 0x10
    header = b"ID3" + bytes([version, 0]) + bytes([flags]) + syncsafe(len(stored))
    tag = header + stored
    if footer:
        tag += b"3DI" + bytes([version, 0]) + bytes([flags]) + syncsafe(len(stored))
    art_off = 10 + art_body_off if art_body_off is not None else None
    return tag, art_off, enc_size


def id3v1_tag(title: str, artist: str, album: str, year: str, comment: str,
              track: int, genre_index: int) -> bytes:
    def pad(text: str, n: int) -> bytes:
        raw = text.encode("latin-1", "replace")[:n]
        return raw + b"\x00" * (n - len(raw))

    if track:
        c = pad(comment, 28) + b"\x00" + bytes([track])
    else:
        c = pad(comment, 30)
    return b"TAG" + pad(title, 30) + pad(artist, 30) + pad(album, 30) + pad(year, 4) + c + bytes([genre_index])


# --------------------------------------------------------------------------- #
# FLAC picture block / Ogg helpers
# --------------------------------------------------------------------------- #
def flac_picture(pic: bytes, mime: str, width: int, height: int, ptype: int = 3) -> bytes:
    depth = 32 if mime == "image/png" else 24
    return (
        be32(ptype) + be32(len(mime)) + mime.encode("ascii") + be32(0)
        + be32(width) + be32(height) + be32(depth) + be32(0) + be32(len(pic)) + pic
    )


def ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def ogg_page(flags: int, granule: int, serial: int, seq: int, segs: bytes, body: bytes) -> bytes:
    hdr = (
        b"OggS" + bytes([0, flags]) + struct.pack("<q", granule) + le32(serial)
        + le32(seq) + b"\x00\x00\x00\x00" + bytes([len(segs)]) + segs
    )
    page = bytearray(hdr + body)
    page[22:26] = le32(ogg_crc(bytes(page)))
    return bytes(page)


def parse_ogg_pages(data: bytes) -> list[dict]:
    pages = []
    p = 0
    while p + 27 <= len(data):
        if data[p:p + 4] != b"OggS":
            raise ValueError(f"bad OggS at {p}")
        nsegs = data[p + 26]
        segs = data[p + 27:p + 27 + nsegs]
        body_off = p + 27 + nsegs
        body_len = sum(segs)
        if body_off + body_len > len(data):
            raise ValueError("truncated ogg page")
        pages.append({
            "off": p, "flags": data[p + 5],
            "granule": struct.unpack("<q", data[p + 6:p + 14])[0],
            "serial": struct.unpack("<I", data[p + 14:p + 18])[0],
            "seq": struct.unpack("<I", data[p + 18:p + 22])[0],
            "segs": bytes(segs), "body_off": body_off,
            "body": data[body_off:body_off + body_len],
        })
        p = body_off + body_len
    return pages


def ogg_header_packets(pages: list[dict], count: int) -> tuple[list[bytes], int]:
    """Extract the first `count` packets and return them plus the file offset of
    the first byte after the last one."""
    packets: list[bytes] = []
    cur = bytearray()
    for idx, page in enumerate(pages):
        pos = 0
        for lace in page["segs"]:
            cur += page["body"][pos:pos + lace]
            pos += lace
            if lace < 255:
                packets.append(bytes(cur))
                cur.clear()
                if len(packets) == count:
                    # A packet that ends exactly at a page boundary means the
                    # first audio byte is in the next page's body.
                    if pos >= len(page["body"]) and idx + 1 < len(pages):
                        return packets, pages[idx + 1]["body_off"]
                    return packets, page["body_off"] + pos
    raise ValueError("not enough ogg header packets")


def ogg_rebuild(path: Path, header_packets: list[bytes], hdr_count: int,
                max_body: int = 65024) -> None:
    """Re-page only the header packets of an existing Ogg file, renumbering the
    untouched audio pages so the stream stays valid."""
    data = path.read_bytes()
    pages = parse_ogg_pages(data)
    serial = pages[0]["serial"]
    old_packets, _ = ogg_header_packets(pages, hdr_count)
    audio = pages[_header_page_count(old_packets, pages):]

    out = bytearray()
    seq = 0
    for pi, pkt in enumerate(header_packets):
        off = 0
        first = True
        while off < len(pkt):
            take = min(len(pkt) - off, max_body)
            if off + take < len(pkt):  # page must end on a 255 lacing value
                take -= take % 255
            segs = bytes([255] * (take // 255))
            if off + take == len(pkt):
                segs += bytes([take % 255])
            flags = 0
            if pi == 0 and first:
                flags |= 0x02  # BOS
            if not first:
                flags |= 0x01  # continued packet
            granule = 0 if off + take == len(pkt) else -1
            out += ogg_page(flags, granule, serial, seq, segs, pkt[off:off + take])
            seq += 1
            off += take
            first = False

    for page in audio:
        out += ogg_page(page["flags"] & ~0x02, page["granule"], serial, seq,
                        page["segs"], page["body"])
        seq += 1
    path.write_bytes(bytes(out))


def _header_page_count(header_packets: list[bytes], pages: list[dict]) -> int:
    """How many leading pages the original header packets occupied."""
    remaining = sum(len(p) for p in header_packets)
    # Walk page bodies until all header bytes are accounted for.
    used = 0
    for i, page in enumerate(pages):
        used += len(page["body"])
        if used >= remaining:
            return i + 1
    raise ValueError("header packets longer than file")


# --------------------------------------------------------------------------- #
# layout probes (mirror of what the C parser must return, independently coded)
# --------------------------------------------------------------------------- #
def art_type(pic: bytes) -> str:
    if len(pic) >= 2 and pic[0] == 0xFF and pic[1] == 0xD8:
        return "jpeg"
    if len(pic) >= 4 and pic[0] == 0x89 and pic[1:4] == b"PNG":
        return "png"
    return "other"


def mp3_layout(data: bytes, picture: bytes | None) -> dict:
    tag_end = 0
    if data[:3] == b"ID3":
        ver = data[3]
        flags = data[5]
        size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
        tag_end = 10 + size + (10 if (ver == 4 and flags & 0x10) else 0)
    end = len(data)
    if len(data) >= 128 and data[end - 128:end - 125] == b"TAG":
        end -= 128
    art = None
    if picture is not None:
        off = data.find(picture)
        if off < 0 or data.find(picture, off + 1) >= 0:
            raise ValueError("picture bytes not uniquely locatable")
        art = {"enc": "raw", "off": off, "size": len(picture), "type": art_type(picture)}
    return {"audio_offset": tag_end, "audio_size": end - tag_end, "art": art}


def flac_layout(data: bytes) -> dict:
    assert data[:4] == b"fLaC"
    p = 4
    art = None
    while True:
        head = data[p]
        last = head & 0x80
        btype = head & 0x7F
        blen = int.from_bytes(data[p + 1:p + 4], "big")
        body = p + 4
        if btype == 6:
            q = body
            ptype = int.from_bytes(data[q:q + 4], "big")
            mlen = int.from_bytes(data[q + 4:q + 8], "big")
            q += 8
            mime = data[q:q + mlen].decode("ascii", "replace")
            q += mlen
            dlen = int.from_bytes(data[q:q + 4], "big")
            q += 4 + dlen
            width = int.from_bytes(data[q:q + 4], "big")
            height = int.from_bytes(data[q + 4:q + 8], "big")
            q += 16  # width, height, depth, colors
            dsize = int.from_bytes(data[q:q + 4], "big")
            q += 4
            if art is None or ptype == 3:
                art = {
                    "enc": "raw", "off": q, "size": dsize,
                    "type": "png" if mime == "image/png" else ("jpeg" if mime == "image/jpeg" else "other"),
                    "width": width, "height": height,
                }
        p = body + blen
        if last:
            break
    return {"audio_offset": p, "audio_size": len(data) - p, "art": art}


def _riff_chunks(data: bytes):
    p = 12
    while p + 8 <= len(data):
        cid = data[p:p + 4]
        size = int.from_bytes(data[p + 4:p + 8], "little")
        yield cid, size, p + 8
        p += 8 + size + (size & 1)


def wav_layout(data: bytes, picture: bytes | None = None) -> dict:
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    audio_offset = audio_size = 0
    art = None
    for cid, size, body in _riff_chunks(data):
        if cid == b"data":
            audio_offset, audio_size = body, size
        elif cid in (b"id3 ", b"ID3 ") and picture is not None:
            off = data.find(picture, body, body + size)
            if off >= 0:
                art = {"enc": "raw", "off": off, "size": len(picture),
                       "type": art_type(picture)}
    return {"audio_offset": audio_offset, "audio_size": audio_size, "art": art}


def ogg_layout(data: bytes, n_headers: int) -> dict:
    pages = parse_ogg_pages(data)
    packets, audio_offset = ogg_header_packets(pages, n_headers)
    comment = packets[1]
    art = None
    # comments are length-prefixed; walk them and map the value back to file offsets
    offs = _ogg_packet_offsets(data, pages, 1, len(comment))
    vlen = int.from_bytes(comment[7:11], "little")
    q = 11 + vlen
    count = int.from_bytes(comment[q:q + 4], "little")
    q += 4
    marker = b"METADATA_BLOCK_PICTURE="
    for _ in range(count):
        ln = int.from_bytes(comment[q:q + 4], "little")
        q += 4
        ent = comment[q:q + ln]
        if ent.startswith(marker):
            val_off = q + len(marker)
            b64 = ent[len(marker):]
            foff = offs[val_off]
            contiguous = offs[q + len(ent) - 1] == foff + len(b64) - 1
            inner_off, inner_size, ptype, width, height = _flac_picture_header_from_b64(b64)
            art = {
                "enc": "base64",
                "off": foff if contiguous else -1,
                "size": len(b64) if contiguous else -1,
                "inner_offset": inner_off, "inner_size": inner_size,
                "type": ptype if contiguous else "other",
                "width": width, "height": height,
            }
        q += ln
    return {"audio_offset": audio_offset, "audio_size": len(data) - audio_offset, "art": art}


def _ogg_packet_offsets(data: bytes, pages: list[dict], packet_index: int,
                        packet_len: int) -> list[int]:
    offs: list[int] = []
    seen = 0
    for page in pages:
        pos = 0
        for lace in page["segs"]:
            if seen == packet_index:
                offs.extend(range(page["body_off"] + pos, page["body_off"] + pos + lace))
            pos += lace
            if lace < 255:
                seen += 1
        if len(offs) >= packet_len:
            break
    return offs


def _flac_picture_header_from_b64(b64: bytes) -> tuple[int, int, str, int, int]:
    raw = base64.b64decode(b64[:512] + b"=" * ((4 - len(b64[:512]) % 4) % 4))
    ptype = int.from_bytes(raw[0:4], "big")
    mlen = int.from_bytes(raw[4:8], "big")
    q = 8 + mlen
    dlen = int.from_bytes(raw[q:q + 4], "big")
    q += 4 + dlen
    width = int.from_bytes(raw[q:q + 4], "big")
    height = int.from_bytes(raw[q + 4:q + 8], "big")
    q += 16
    dsize = int.from_bytes(raw[q:q + 4], "big")
    q += 4
    mime = raw[8:8 + mlen]
    ptype_s = "png" if mime == b"image/png" else ("jpeg" if mime == b"image/jpeg" else "other")
    return q, dsize, ptype_s, width, height


# --------------------------------------------------------------------------- #
# audio bases
# --------------------------------------------------------------------------- #
def gen_audio_bases(tmp: Path) -> dict:
    bases = {}

    def mp3(name, extra_args):
        p = tmp / name
        run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
             f"sine=frequency=440:duration={CLIP_SECONDS}", "-ac", "2", "-ar", str(RATE),
             "-c:a", "libmp3lame", *extra_args, "-map_metadata", "-1",
             "-id3v2_version", "0", "-write_id3v1", "0", str(p)])
        bases[name] = p

    mp3("base_cbr.mp3", ["-b:a", "192k"])
    mp3("base_vbr.mp3", ["-q:a", "4"])

    p = tmp / "base.wav"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=440:duration={CLIP_SECONDS}", "-ac", "2", "-ar", str(RATE),
         "-c:a", "pcm_s16le", "-map_metadata", "-1", "-write_id3v2", "0", str(p)])
    bases["base.wav"] = p
    return bases


# --------------------------------------------------------------------------- #
# record keeping
# --------------------------------------------------------------------------- #
RECORDS: list[dict] = []


def record(name: str, *, fmt: str, tags: dict, layout: dict,
           art: dict | None = None, notes: str = "") -> None:
    rec = {
        "file": name,
        "format": fmt,
        "notes": notes,
        "tags": {
            "title": tags.get("title", ""),
            "artist": tags.get("artist", ""),
            "album": tags.get("album", ""),
            "album_artist": tags.get("album_artist", ""),
            "genre": tags.get("genre", ""),
            "track": tags.get("track", 0),
            "track_total": tags.get("track_total", 0),
            "disc": tags.get("disc", 0),
            "disc_total": tags.get("disc_total", 0),
            "year": tags.get("year", 0),
        },
        "expect": {
            "title": utf8_trunc(tags.get("title", ""), 64),
            "artist": utf8_trunc(tags.get("artist", ""), 48),
            "album": utf8_trunc(tags.get("album", ""), 48),
            "album_artist": utf8_trunc(tags.get("album_artist", ""), 48),
            "genre": utf8_trunc(tags.get("genre", ""), 32),
            "track": tags.get("track", 0),
            "track_total": tags.get("track_total", 0),
            "disc": tags.get("disc", 0),
            "disc_total": tags.get("disc_total", 0),
            "year": tags.get("year", 0),
            "audio_offset": layout["audio_offset"],
            "audio_size": layout["audio_size"],
            "art": art,
        },
    }
    RECORDS.append(rec)


def finish(path: Path, rec: dict) -> None:
    """ffprobe the finished file and fill the measured fields."""
    probe = ffprobe(path)
    rec["ffprobe"] = probe
    rec["expect"]["duration_ms"] = probe["duration_ms"]
    rec["expect"]["sample_rate"] = probe["sample_rate"]
    rec["expect"]["channels"] = probe["channels"]
    rec["expect"]["bits"] = probe["bits"]
    # average audio bitrate of the payload (what the firmware reports)
    dur = probe["duration_ms"]
    size = rec["expect"]["audio_size"]
    rec["expect"]["bitrate_kbps"] = int(round(size * 8.0 / dur)) if dur else 0
    rec["ffprobe"]["tags_seen"] = ffprobe_tags(path)

    # validate the file really decodes (catches a broken hand-written container)
    run([FFMPEG, "-v", "error", "-i", str(path), "-f", "null", "-"])
    print(f"  {path.name:32s} {probe['codec']:8s} {dur:5d} ms  "
          f"{rec['expect']['bitrate_kbps']:4d} kbps  art={bool(rec['expect']['art'])}")


# --------------------------------------------------------------------------- #
# tags used by every file (all fictional)
# --------------------------------------------------------------------------- #
T1 = dict(title="Neon Meridian", artist="The Fictional Hours", album="Paper Compass",
          album_artist="The Fictional Hours", genre="Synthwave",
          track=3, track_total=12, disc=1, disc_total=2, year=2021)
T2 = dict(title="Ünïcode VBR Ångström", artist="Kōji Überset", album="Vörös Könyv",
          album_artist="Kōji Überset", genre="Dreampop",
          track=2, track_total=7, disc=1, disc_total=1, year=2024)
LONG_TITLE = ("The Cartographer's Long Farewell (Extended Reprise) "
              "— 地図師の長い別れ (Remastered Edition)")
T3 = dict(title=LONG_TITLE, artist="Héloïse Brünn 東京", album="Über Den Wolken",
          album_artist="Héloïse Brünn", genre="Ambient",
          track=9, track_total=14, disc=2, disc_total=3, year=2017)
T4 = dict(title="Cassette Ghost", artist="Analog Youth", album="Rewind Season",
          album_artist="Analog Youth", genre="Pop",  # written as TCO "(13)"
          track=6, track_total=10, disc=0, disc_total=0, year=1998)
T5 = dict(title="Static Bloom", artist="Sÿnth Valley", album="Filtered Air",
          album_artist="Sÿnth Valley", genre="Electronic",
          track=11, track_total=11, disc=1, disc_total=1, year=2015)
T6 = dict(title="Dust Highway", artist="Roadside Choir", album="Mile Marker",
          album_artist="", genre="Rock",
          track=7, track_total=0, disc=0, disc_total=0, year=2005)
T8 = dict(title="Glass Harbour", artist="Marina Vale", album="Tidal Papers",
          album_artist="Marina Vale", genre="Ambient",
          track=7, track_total=9, disc=1, disc_total=1, year=2019)
T9 = dict(title="Moss Garden", artist="Quiet Fern", album="Green Static",
          album_artist="Quiet Fern", genre="Drone",
          track=1, track_total=4, disc=0, disc_total=0, year=2020)
T10 = dict(title="Copper Lake", artist="Fen & Field", album="Field Notes",
           album_artist="", genre="Folk",
           track=2, track_total=10, disc=0, disc_total=0, year=2020)
T11 = dict(title="Paper Kite", artist="Ivy Section", album="Small Hours",
           album_artist="Ivy Section", genre="Indie",
           track=5, track_total=11, disc=1, disc_total=1, year=2018)
T12 = dict(title="Aurora Diner", artist="Pale Static", album="Night Menu",
           album_artist="Pale Static", genre="Dreampop",
           track=4, track_total=8, disc=0, disc_total=0, year=2022)
T13 = dict(title="Quiet Machines", artist="Grey Transit", album="Slow Lines",
           album_artist="", genre="Electronic",
           track=1, track_total=6, disc=0, disc_total=0, year=2023)
T14 = dict(title="Signal Garden", artist="Long Page", album="Split Comments",
           album_artist="Long Page", genre="Ambient",
           track=1, track_total=2, disc=0, disc_total=0, year=2025)


# --------------------------------------------------------------------------- #
# main generation
# --------------------------------------------------------------------------- #
def main() -> int:
    if not FFMPEG or not FFPROBE:
        print("ffmpeg/ffprobe not found on PATH", file=sys.stderr)
        return 1
    OUT.mkdir(exist_ok=True)
    covers = make_covers(OUT)
    tmp = Path(tempfile.mkdtemp(prefix="gen_test_media_"))
    bases = gen_audio_bases(tmp)
    c300 = covers["cover_300.jpg"]
    c500 = covers["cover_500.jpg"]
    c400 = covers["cover_400x300.jpg"]
    cpng = covers["cover_300.png"]

    print("writing test_media/ ...")

    # 1. MP3: CBR, ID3v2.3 + ID3v1 + APIC (ffmpeg writes Latin-1/UTF-16 as needed)
    p = OUT / "mp3_v23_id3v1_apic.mp3"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=440:duration={CLIP_SECONDS}", "-i", str(c300["path"]), "-map", "0:a", "-map", "1:v", "-ac", "2", "-ar", str(RATE), "-c:a", "libmp3lame",
         "-b:a", "192k", "-c:v", "copy", "-id3v2_version", "3", "-write_id3v1", "1",
         "-metadata", f"title={T1['title']}", "-metadata", f"artist={T1['artist']}",
         "-metadata", f"album={T1['album']}", "-metadata", f"album_artist={T1['album_artist']}",
         "-metadata", f"genre={T1['genre']}", "-metadata", "track=3/12",
         "-metadata", "disc=1/2", "-metadata", "date=2021",
         "-metadata:s:v", "title=Cover (front)", "-metadata:s:v", "comment=Cover (front)",
         str(p)])
    lay = mp3_layout(p.read_bytes(), c300["bytes"])
    record(p.name, fmt="mp3", tags=T1, layout=lay, art=lay["art"],
           notes="ID3v2.3 (ffmpeg) + ID3v1.1 + APIC image/jpeg type 3")
    finish(p, RECORDS[-1])

    # 2. MP3: VBR + ID3v2.4 UTF-8 + APIC + footer (footer added by hand)
    p = OUT / "mp3_vbr_v24_utf8.mp3"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=330:duration={CLIP_SECONDS}", "-i", str(c500["path"]), "-map", "0:a", "-map", "1:v", "-ac", "2", "-ar", str(RATE), "-c:a", "libmp3lame",
         "-q:a", "4", "-c:v", "copy", "-id3v2_version", "4", "-write_id3v1", "0",
         "-metadata", f"title={T2['title']}", "-metadata", f"artist={T2['artist']}",
         "-metadata", f"album={T2['album']}", "-metadata", f"album_artist={T2['album_artist']}",
         "-metadata", f"genre={T2['genre']}", "-metadata", "track=2/7",
         "-metadata", "disc=1/1", "-metadata", "date=2024",
         "-metadata:s:v", "title=Cover (front)", "-metadata:s:v", "comment=Cover (front)",
         str(p)])
    raw = bytearray(p.read_bytes())
    assert raw[0:3] == b"ID3" and raw[3] == 4
    size = (raw[6] << 21) | (raw[7] << 14) | (raw[8] << 7) | raw[9]
    raw[5] |= 0x10  # footer present
    footer = b"3DI" + bytes(raw[3:5]) + bytes([raw[5]]) + raw[6:10]
    p.write_bytes(bytes(raw[:10 + size]) + footer + bytes(raw[10 + size:]))
    lay = mp3_layout(p.read_bytes(), c500["bytes"])
    record(p.name, fmt="mp3", tags=T2, layout=lay, art=lay["art"],
           notes="ID3v2.4 UTF-8 (ffmpeg) + hand-added footer, VBR Xing, APIC jpeg")
    finish(p, RECORDS[-1])

    # 3. MP3: hand-written ID3v2.3 UTF-16 (BOM) + extended header + APIC
    p = OUT / "mp3_v23_utf16.mp3"
    apic, apic_off = apic_payload(c400)
    frames = [
        (b"TIT2", id3_text(1, T3["title"]), b"\x00\x00"),
        (b"TPE1", id3_text(1, T3["artist"]), b"\x00\x00"),
        (b"TALB", id3_text(1, T3["album"]), b"\x00\x00"),
        (b"TPE2", id3_text(1, T3["album_artist"]), b"\x00\x00"),
        (b"TCON", id3_text(1, T3["genre"]), b"\x00\x00"),
        (b"TRCK", id3_text(1, "9/14"), b"\x00\x00"),
        (b"TPOS", id3_text(1, "2/3"), b"\x00\x00"),
        (b"TYER", id3_text(1, "2017"), b"\x00\x00"),
        (b"APIC", apic, b"\x00\x00"),
    ]
    ext = be32(6) + b"\x00\x00" + be32(0)  # v2.3 ext header, size 6, no padding
    tag, art_off, art_size = build_id3v2(3, frames, extended_header=ext,
                                         art_payload_off=apic_off,
                                         art_frame_index=len(frames) - 1)
    p.write_bytes(tag + bases["base_cbr.mp3"].read_bytes())
    lay = mp3_layout(p.read_bytes(), None)
    lay["art"] = {"enc": "raw", "off": art_off, "size": art_size, "type": "jpeg"}
    record(p.name, fmt="mp3", tags=T3, layout=lay, art=lay["art"],
           notes="hand-written ID3v2.3: extended header, UTF-16LE+BOM frames, "
                 "non-ASCII + CJK, long title, APIC jpeg 400x300")
    finish(p, RECORDS[-1])

    # 4. MP3: ID3v2.2 (PIC frame), numeric genre "(13)"
    p = OUT / "mp3_v22.mp3"
    pic, pic_off = pic_payload(c300)
    frames = [
        (b"TT2", id3_text(0, T4["title"]), b""),
        (b"TP1", id3_text(0, T4["artist"]), b""),
        (b"TAL", id3_text(0, T4["album"]), b""),
        (b"TP2", id3_text(0, T4["album_artist"]), b""),
        (b"TCO", id3_text(0, "(13)"), b""),
        (b"TRK", id3_text(0, "6/10"), b""),
        (b"TYE", id3_text(0, "1998"), b""),
        (b"PIC", pic, b""),
    ]
    tag, art_off, art_size = build_id3v2(2, frames, art_payload_off=pic_off,
                                         art_frame_index=len(frames) - 1)
    p.write_bytes(tag + bases["base_vbr.mp3"].read_bytes())
    lay = mp3_layout(p.read_bytes(), None)
    lay["art"] = {"enc": "raw", "off": art_off, "size": art_size, "type": "jpeg"}
    record(p.name, fmt="mp3", tags=T4, layout=lay, art=lay["art"],
           notes="ID3v2.2 (TT2/TP1/TAL/TP2/TCO/TRK/TYE/PIC), genre '(13)' -> Pop")
    finish(p, RECORDS[-1])

    # 5. MP3: whole-tag unsynchronisation, art full of 0xFF, Latin-1 0xFF text
    p = OUT / "mp3_unsync.mp3"
    apic, apic_off = apic_payload(c300)
    frames = [
        (b"TIT2", id3_text(0, T5["title"]), b"\x00\x00"),
        (b"TPE1", id3_text(0, T5["artist"]), b"\x00\x00"),  # contains 0xFF (ÿ)
        (b"TALB", id3_text(0, T5["album"]), b"\x00\x00"),
        (b"TPE2", id3_text(0, T5["album_artist"]), b"\x00\x00"),
        (b"TCON", id3_text(0, T5["genre"]), b"\x00\x00"),
        (b"TRCK", id3_text(0, "11/11"), b"\x00\x00"),
        (b"TPOS", id3_text(0, "1/1"), b"\x00\x00"),
        (b"TYER", id3_text(0, "2015"), b"\x00\x00"),
        (b"APIC", apic, b"\x00\x00"),
    ]
    tag, art_off, art_size = build_id3v2(3, frames, tag_unsync=True,
                                         art_payload_off=apic_off,
                                         art_frame_index=len(frames) - 1)
    p.write_bytes(tag + bases["base_cbr.mp3"].read_bytes())
    lay = mp3_layout(p.read_bytes(), None)
    lay["art"] = {"enc": "id3_unsync", "off": art_off, "size": art_size, "type": "jpeg"}
    record(p.name, fmt="mp3", tags=T5, layout=lay, art=lay["art"],
           notes="ID3v2.3 whole-tag unsynchronisation (flag 0x80), 0xFF-laden "
                 "JPEG art, Latin-1 0xFF in TPE1")
    finish(p, RECORDS[-1])

    # 6. MP3: ID3v1 only
    p = OUT / "mp3_id3v1_only.mp3"
    p.write_bytes(bases["base_cbr.mp3"].read_bytes()
                  + id3v1_tag("Dust Highway", "Roadside Choir", "Mile Marker",
                              "2005", "spare", 7, 17))
    lay = mp3_layout(p.read_bytes(), None)
    record(p.name, fmt="mp3", tags=T6, layout=lay, art=None,
           notes="ID3v1.1 only (title/artist/album/year/comment/track/genre=17 Rock)")
    finish(p, RECORDS[-1])

    # 7. MP3: no tags
    p = OUT / "mp3_notags.mp3"
    p.write_bytes(bases["base_vbr.mp3"].read_bytes())
    lay = mp3_layout(p.read_bytes(), None)
    record(p.name, fmt="mp3", tags={"title": p.stem}, layout=lay, art=None,
           notes="no ID3 at all; title falls back to the file name")
    finish(p, RECORDS[-1])

    # 8. FLAC: Vorbis comments + PICTURE (JPEG)
    p = OUT / "flac_vorbis_jpeg.flac"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=440:duration={CLIP_SECONDS}", "-i", str(c300["path"]), "-map", "0:a", "-map", "1:v", "-ac", "2", "-ar", str(RATE), "-c:a", "flac",
         "-sample_fmt", "s16", "-c:v", "copy", "-disposition:v", "attached_pic",
         "-metadata", f"title={T8['title']}", "-metadata", f"artist={T8['artist']}",
         "-metadata", f"album={T8['album']}", "-metadata", f"album_artist={T8['album_artist']}",
         "-metadata", f"genre={T8['genre']}", "-metadata", "track=7/9",
         "-metadata", "disc=1/1", "-metadata", "date=2019-04-02",
         str(p)])
    lay = flac_layout(p.read_bytes())
    record(p.name, fmt="flac", tags=T8, layout=lay, art=lay["art"],
           notes="FLAC STREAMINFO + VORBIS_COMMENT + PICTURE image/jpeg")
    finish(p, RECORDS[-1])

    # 9. FLAC: PNG picture
    p = OUT / "flac_png_art.flac"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=520:duration={CLIP_SECONDS}", "-i", str(cpng["path"]), "-map", "0:a", "-map", "1:v", "-ac", "2", "-ar", str(RATE), "-c:a", "flac",
         "-sample_fmt", "s16", "-c:v", "copy", "-disposition:v", "attached_pic",
         "-metadata", f"title={T9['title']}", "-metadata", f"artist={T9['artist']}",
         "-metadata", f"album={T9['album']}", "-metadata", f"album_artist={T9['album_artist']}",
         "-metadata", f"genre={T9['genre']}", "-metadata", "track=1/4",
         "-metadata", "date=2020",
         str(p)])
    lay = flac_layout(p.read_bytes())
    record(p.name, fmt="flac", tags=T9, layout=lay, art=lay["art"],
           notes="FLAC PICTURE image/png")
    finish(p, RECORDS[-1])

    # 10. WAV: LIST INFO (ffmpeg), PCM s16le stereo
    p = OUT / "wav_list_info.wav"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=523.25:duration={CLIP_SECONDS}", "-ac", "2", "-ar", str(RATE),
         "-c:a", "pcm_s16le", "-write_id3v2", "0",
         "-metadata", f"title={T10['title']}", "-metadata", f"artist={T10['artist']}",
         "-metadata", f"album={T10['album']}", "-metadata", f"genre={T10['genre']}",
         "-metadata", "track=2/10", "-metadata", "date=2020", str(p)])
    data = p.read_bytes()
    assert data.find(b"LIST") >= 0, "ffmpeg did not write LIST INFO"
    lay = wav_layout(data)
    record(p.name, fmt="wav", tags=T10, layout=lay, art=None,
           notes="RIFF/WAVE PCM s16le stereo, LIST INFO (INAM/IART/IPRD/IGNR/ICRD/IPRT)")
    finish(p, RECORDS[-1])

    # 11. WAV: `id3 ` chunk with a hand-written ID3v2.3 tag + APIC
    p = OUT / "wav_id3_chunk.wav"
    apic, apic_off = apic_payload(c300)
    frames = [
        (b"TIT2", id3_text(0, T11["title"]), b"\x00\x00"),
        (b"TPE1", id3_text(0, T11["artist"]), b"\x00\x00"),
        (b"TALB", id3_text(0, T11["album"]), b"\x00\x00"),
        (b"TPE2", id3_text(0, T11["album_artist"]), b"\x00\x00"),
        (b"TCON", id3_text(0, T11["genre"]), b"\x00\x00"),
        (b"TRCK", id3_text(0, "5/11"), b"\x00\x00"),
        (b"TPOS", id3_text(0, "1/1"), b"\x00\x00"),
        (b"TYER", id3_text(0, "2018"), b"\x00\x00"),
        (b"APIC", apic, b"\x00\x00"),
    ]
    tag, _, _ = build_id3v2(3, frames, art_payload_off=apic_off,
                            art_frame_index=len(frames) - 1)
    base = bytearray(bases["base.wav"].read_bytes())
    base += b"id3 " + le32(len(tag)) + tag + (b"\x00" if len(tag) & 1 else b"")
    base[4:8] = le32(len(base) - 8)
    p.write_bytes(bytes(base))
    lay = wav_layout(p.read_bytes(), c300["bytes"])
    record(p.name, fmt="wav", tags=T11, layout=lay, art=lay["art"],
           notes="RIFF/WAVE with a trailing `id3 ` chunk (ID3v2.3) and APIC; no LIST")
    finish(p, RECORDS[-1])

    # 12. Ogg Vorbis: comments + METADATA_BLOCK_PICTURE (base64 JPEG)
    p = OUT / "ogg_vorbis_art.ogg"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=392:duration={CLIP_SECONDS}", "-ac", "2", "-ar", str(RATE),
         "-c:a", "libvorbis", "-q:a", "3",
         "-metadata", f"title={T12['title']}", "-metadata", f"artist={T12['artist']}",
         "-metadata", f"album={T12['album']}", "-metadata", f"album_artist={T12['album_artist']}",
         "-metadata", f"genre={T12['genre']}", "-metadata", "track=4/8",
         "-metadata", "date=2022", str(p)])
    _ogg_add_picture(p, c300)
    lay = ogg_layout(p.read_bytes(), 3)
    record(p.name, fmt="ogg_vorbis", tags=T12, layout=lay, art=lay["art"],
           notes="Ogg Vorbis: id header + comment header with METADATA_BLOCK_PICTURE "
                 "base64 (JPEG, type 3), re-paged by the generator")
    finish(p, RECORDS[-1])

    # 12b. Ogg Vorbis: picture whose base64 value spans several Ogg pages.
    #      The parser must keep streaming (no whole-packet buffer) and report
    #      encoding=base64 with type=other / offset=size=0 because the value
    #      cannot be expressed as one contiguous file range.
    p = OUT / "ogg_vorbis_art_spanning.ogg"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=466:duration={CLIP_SECONDS}", "-ac", "2", "-ar", str(RATE),
         "-c:a", "libvorbis", "-q:a", "3",
         "-metadata", f"title={T14['title']}", "-metadata", f"artist={T14['artist']}",
         "-metadata", f"album={T14['album']}", "-metadata", f"album_artist={T14['album_artist']}",
         "-metadata", f"genre={T14['genre']}", "-metadata", "track=1/2",
         "-metadata", "date=2025", str(p)])
    _ogg_add_picture(p, c500, max_body=2048)
    lay = ogg_layout(p.read_bytes(), 3)
    assert lay["art"] is not None and lay["art"]["off"] == -1
    record(p.name, fmt="ogg_vorbis", tags=T14, layout=lay, art=lay["art"],
           notes="Ogg Vorbis: METADATA_BLOCK_PICTURE base64 split over ~14 pages "
                 "(2048-byte pages); parser streams it but reports offset/size -1, "
                 "type other (see README)")
    finish(p, RECORDS[-1])

    # 13. Ogg Opus: comments, no art
    p = OUT / "ogg_opus.opus"
    run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency=294:duration={CLIP_SECONDS}", "-ac", "2", "-ar", "48000",
         "-c:a", "libopus", "-b:a", "96k",
         "-metadata", f"title={T13['title']}", "-metadata", f"artist={T13['artist']}",
         "-metadata", f"album={T13['album']}", "-metadata", f"genre={T13['genre']}",
         "-metadata", "track=1/6", "-metadata", "date=2023", str(p)])
    lay = ogg_layout(p.read_bytes(), 2)
    record(p.name, fmt="ogg_opus", tags=T13, layout=lay, art=None,
           notes="Ogg Opus: OpusHead (pre-skip) + OpusTags, no art")
    finish(p, RECORDS[-1])

    write_expected(covers)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{len(RECORDS)} files -> {OUT}")
    return 0


def _ogg_add_picture(path: Path, cover: dict, max_body: int = 65024) -> None:
    """Inject METADATA_BLOCK_PICTURE into an existing Ogg Vorbis comment packet."""
    data = path.read_bytes()
    pages = parse_ogg_pages(data)
    packets, _ = ogg_header_packets(pages, 3)
    ident, comment, setup = packets
    assert comment[:7] == b"\x03vorbis"
    vlen = int.from_bytes(comment[7:11], "little")
    vendor = comment[11:11 + vlen]
    q = 11 + vlen
    count = int.from_bytes(comment[q:q + 4], "little")
    q += 4
    entries = []
    for _ in range(count):
        ln = int.from_bytes(comment[q:q + 4], "little")
        entries.append(comment[q + 4:q + 4 + ln])
        q += 4 + ln
    picture = flac_picture(cover["bytes"], cover["mime"], cover["width"], cover["height"])
    entries.append(b"METADATA_BLOCK_PICTURE=" + base64.b64encode(picture))
    new_comment = bytearray(b"\x03vorbis" + le32(len(vendor)) + vendor + le32(len(entries)))
    for e in entries:
        new_comment += le32(len(e)) + e
    ogg_rebuild(path, [ident, bytes(new_comment), setup], 3, max_body=max_body)


def write_expected(covers: dict) -> None:
    files = []
    for rec in RECORDS:
        files.append(rec)
    doc = {
        "generated_by": "scripts/gen_test_media.py",
        "clip_seconds": CLIP_SECONDS,
        "notes": ("Tags are authoritative (as written). ffprobe values are measured; "
                  "expect.* is what lib/media/media_meta.c must report. Art offsets are "
                  "absolute file offsets of the *encoded* picture bytes (raw / "
                  "id3-unsynchronised / base64 text). Art width/height are 0 when the "
                  "container does not carry them (ID3 APIC/PIC)."),
        "covers": {name: {k: v for k, v in c.items() if k != "bytes" and k != "path"}
                   for name, c in covers.items()},
        "files": files,
    }
    (OUT / "expected.json").write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                                       encoding="utf-8")

    cols = ["name", "format", "title", "artist", "album", "album_artist", "genre",
            "track", "track_total", "disc", "disc_total", "year",
            "duration_ms", "sample_rate", "channels", "bits", "bitrate_kbps",
            "audio_offset", "audio_size", "art_enc", "art_type", "art_off", "art_size",
            "art_w", "art_h", "inner_off", "inner_size"]
    lines = ["\t".join(cols)]
    for rec in files:
        e = rec["expect"]
        art = e["art"] or {}
        vals = [
            rec["file"], rec["format"], e["title"], e["artist"], e["album"],
            e["album_artist"], e["genre"], e["track"], e["track_total"], e["disc"],
            e["disc_total"], e["year"], e["duration_ms"], e["sample_rate"],
            e["channels"], e["bits"], e["bitrate_kbps"], e["audio_offset"],
            e["audio_size"], art.get("enc", "none"), art.get("type", "none"),
            art.get("off", -1), art.get("size", -1), art.get("width", 0),
            art.get("height", 0), art.get("inner_offset", 0), art.get("inner_size", 0),
        ]
        lines.append("\t".join(tsv_escape(str(v)) for v in vals))
    (OUT / "expected.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")


def tsv_escape(s: str) -> str:
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", ""))


if __name__ == "__main__":
    sys.exit(main())
