"""Hierarchical KiCad 10 schematic generator.

Python 3.11, standard library only.

Public API
----------
    p = Project(name, outdir, title, rev, company="", date="")
    s = p.sheet(name, filename, page_size="A3")
    s.part(ref, sym, value, fp, nets, mpn="", mfr="", dnp=False, nc=(),
           desc="", fields=None, group=None)
    s.note(text)
    s.pwr_flag(net)
    p.custom_symbol(name, pins, footprint="", description="", reference="U")
    p.write()
    p.export_bom(path)

Everything is written as KiCad S-expressions (version 20260306 for
schematics, 20251024 for symbol libraries).  All UUIDs are derived with
uuid5, so regenerating a project produces byte-identical files.
"""

from __future__ import annotations

import copy
import csv
import json
import math
import os
import uuid
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------
# KiCad installation layout
# --------------------------------------------------------------------------

KICAD_VERSION = "20260306"
KICAD_GENERATOR_VERSION = "10.0"
KICAD_SYMBOL_LIB_VERSION = "20251024"


def _find_kicad_root() -> Path:
    env = os.environ.get("KICAD_ROOT")
    if env:
        return Path(env)
    candidates = [
        Path(r"C:\Program Files\KiCad\10.0"),
        Path(r"C:\Program Files\KiCad\9.0"),
        Path("/usr/share/kicad"),
        Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport"),
    ]
    for cand in candidates:
        if cand.is_dir():
            return cand
    return candidates[0]


KICAD_ROOT = _find_kicad_root()
KICAD_SYMBOL_DIR = KICAD_ROOT / "share" / "kicad" / "symbols"
KICAD_FOOTPRINT_DIR = KICAD_ROOT / "share" / "kicad" / "footprints"
KICAD_CLI = KICAD_ROOT / "bin" / ("kicad-cli.exe" if os.name == "nt" else "kicad-cli")


# --------------------------------------------------------------------------
# S-expression reading / writing
# --------------------------------------------------------------------------


class QStr(str):
    """An atom that is always written as a quoted KiCad string."""


_ESCAPE_IN = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}


def _unescape(raw: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch == "\\" and i + 1 < len(raw):
            out.append(_ESCAPE_IN.get(raw[i + 1], raw[i + 1]))
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def tokenize(text: str) -> list:
    tokens: list = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if ch in "()":
            tokens.append(ch)
            i += 1
            continue
        if ch == '"':
            i += 1
            buf: list[str] = []
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    buf.append(text[i : i + 2])
                    i += 2
                    continue
                if text[i] == '"':
                    break
                buf.append(text[i])
                i += 1
            if i >= n:
                raise ValueError("unterminated string in S-expression")
            tokens.append(QStr(_unescape("".join(buf))))
            i += 1
            continue
        j = i
        while j < n and text[j] not in ' \t\r\n()"':
            j += 1
        tokens.append(text[i:j])
        i = j
    return tokens


class SExprError(ValueError):
    pass


def parse_sexpr(text: str) -> list:
    """Parse text into a list of nested lists / atoms.  Quoted atoms are QStr."""
    tokens = tokenize(text)
    pos = 0
    total = len(tokens)

    def read():
        nonlocal pos
        if pos >= total:
            raise SExprError("unexpected end of S-expression")
        tok = tokens[pos]
        pos += 1
        if tok == "(":
            items: list = []
            while pos < total and tokens[pos] != ")":
                items.append(read())
            if pos >= total:
                raise SExprError("missing ')' in S-expression")
            pos += 1
            return items
        if tok == ")":
            raise SExprError("unexpected ')' in S-expression")
        return tok

    roots: list = []
    while pos < total:
        roots.append(read())
    return roots


def parse_sexpr_one(text: str) -> list:
    roots = parse_sexpr(text)
    if len(roots) != 1 or not isinstance(roots[0], list):
        raise SExprError("expected exactly one top-level S-expression")
    return roots[0]


def _atom_text(atom) -> str:
    if isinstance(atom, QStr):
        text = str(atom)
        text = text.replace("\\", "\\\\").replace('"', '\\"')
        text = text.replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")
        return '"' + text + '"'
    return str(atom)


def emit(node, level: int = 0) -> str:
    """Serialize an S-expression node using KiCad-style tab indentation."""
    if not isinstance(node, list):
        return _atom_text(node)
    pad = "\t" * level
    if all(not isinstance(child, list) for child in node):
        return pad + "(" + " ".join(_atom_text(child) for child in node) + ")"
    first_list = next(i for i, child in enumerate(node) if isinstance(child, list))
    line = pad + "(" + " ".join(_atom_text(child) for child in node[:first_list])
    lines = [line]
    for child in node[first_list:]:
        if isinstance(child, list):
            lines.append(emit(child, level + 1))
        else:  # pragma: no cover - KiCad never puts atoms after sub-lists
            lines[-1] += " " + _atom_text(child)
    lines.append(pad + ")")
    return "\n".join(lines)


def emit_document(node) -> str:
    return emit(node, 0) + "\n"


# --------------------------------------------------------------------------
# Small AST helpers
# --------------------------------------------------------------------------


def L(*items) -> list:
    return list(items)


def Q(text: str) -> QStr:
    return QStr(str(text))


def node_head(node):
    if isinstance(node, list) and node:
        return node[0]
    return None


def children(node, head: str) -> list:
    if not isinstance(node, list):
        return []
    return [c for c in node[1:] if node_head(c) == head]


def find_child(node, head: str):
    if not isinstance(node, list):
        return None
    for child in node[1:]:
        if node_head(child) == head:
            return child
    return None


def prop_name(node) -> str | None:
    if node_head(node) != "property" or len(node) < 3:
        return None
    if node[1] == "private":
        return str(node[2]) if len(node) > 2 else None
    return str(node[1])


def prop_value(node):
    if node_head(node) != "property":
        return None
    idx = 3 if node[1] == "private" else 2
    return node[idx] if len(node) > idx else None


def property_value(node, name: str, default: str = "") -> str:
    for prop in children(node, "property"):
        if prop_name(prop) == name:
            value = prop_value(prop)
            return str(value) if value is not None else default
    return default


def _f(token) -> float:
    try:
        return float(str(token))
    except (TypeError, ValueError):
        return 0.0


def fmt(value) -> str:
    """Format a coordinate / number the way KiCad does (no exponent)."""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int):
        return str(value)
    number = float(value)
    if abs(number) < 5e-10:
        return "0"
    text = f"{number:.4f}".rstrip("0").rstrip(".")
    if text in ("", "-0"):
        return "0"
    return text


def _text_width(text: str, size: float = 1.27) -> float:
    return len(str(text)) * size * 0.85 + size * 0.4


# --------------------------------------------------------------------------
# Deterministic UUIDs
# --------------------------------------------------------------------------

_UUID_NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "https://banyan.example/kicadgen")


def make_uuid(*parts) -> str:
    return str(uuid.uuid5(_UUID_NAMESPACE, "/".join(str(p) for p in parts)))


# --------------------------------------------------------------------------
# Geometry
# --------------------------------------------------------------------------


class Box:
    __slots__ = ("xmin", "ymin", "xmax", "ymax")

    def __init__(self, xmin=math.inf, ymin=math.inf, xmax=-math.inf, ymax=-math.inf):
        self.xmin = xmin
        self.ymin = ymin
        self.xmax = xmax
        self.ymax = ymax

    def add(self, x: float, y: float) -> None:
        if x < self.xmin:
            self.xmin = x
        if x > self.xmax:
            self.xmax = x
        if y < self.ymin:
            self.ymin = y
        if y > self.ymax:
            self.ymax = y

    def union(self, other: "Box") -> "Box":
        self.add(other.xmin, other.ymin)
        self.add(other.xmax, other.ymax)
        return self

    def expand(self, dx: float, dy: float | None = None) -> "Box":
        if dy is None:
            dy = dx
        self.xmin -= dx
        self.xmax += dx
        self.ymin -= dy
        self.ymax += dy
        return self

    @property
    def width(self) -> float:
        return self.xmax - self.xmin

    @property
    def height(self) -> float:
        return self.ymax - self.ymin

    def is_empty(self) -> bool:
        return self.xmin > self.xmax or self.ymin > self.ymax


# --------------------------------------------------------------------------
# Symbol libraries
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class PinDef:
    number: str
    name: str
    etype: str
    x: float
    y: float
    rot: int
    length: float


def _iter_pin_nodes(node):
    for child in node[1:]:
        if not isinstance(child, list):
            continue
        head = node_head(child)
        if head == "pin":
            yield child
        elif head == "symbol":
            yield from _iter_pin_nodes(child)


def _pin_from_node(node) -> PinDef:
    at = find_child(node, "at")
    name_node = find_child(node, "name")
    number_node = find_child(node, "number")
    length_node = find_child(node, "length")
    x = _f(at[1]) if at else 0.0
    y = _f(at[2]) if at else 0.0
    rot = int(_f(at[3])) if at and len(at) > 3 else 0
    length = _f(length_node[1]) if length_node else 0.0
    name = str(name_node[1]) if name_node else ""
    number = str(number_node[1]) if number_node else ""
    etype = str(node[1]) if len(node) > 1 else "passive"
    return PinDef(number, name, etype, x, y, rot, length)


def symbol_pins(symbol: list) -> list[PinDef]:
    """All pins of a flattened symbol (duplicates by number are kept)."""
    seen = set()
    pins: list[PinDef] = []
    for pin_node in _iter_pin_nodes(symbol):
        pin = _pin_from_node(pin_node)
        key = (pin.number, pin.x, pin.y, pin.rot, pin.length)
        if key in seen:
            continue
        seen.add(key)
        pins.append(pin)
    return pins


class LibraryResolver:
    """Loads KiCad symbol libraries and flattens ``extends`` inheritance."""

    def __init__(self, symbol_dir: Path | str | None = None):
        self.symbol_dir = Path(symbol_dir) if symbol_dir else KICAD_SYMBOL_DIR
        self._libraries: dict[str, dict[str, list]] = {}
        self._flat: dict[tuple[str, str], list] = {}
        self._custom: dict[str, dict[str, list]] = {}
        self._stack: set[tuple[str, str]] = set()

    # -- custom libraries ---------------------------------------------------
    def add_custom_symbol(self, lib: str, name: str, node: list) -> None:
        self._custom.setdefault(lib, {})[name] = node
        self._flat.pop((lib, name), None)

    # -- library loading ----------------------------------------------------
    def _load(self, lib: str) -> dict[str, list]:
        if lib in self._custom:
            return self._custom[lib]
        if lib in self._libraries:
            return self._libraries[lib]
        path = self.symbol_dir / f"{lib}.kicad_sym"
        if not path.is_file():
            raise FileNotFoundError(f"symbol library not found: {path}")
        root = parse_sexpr_one(path.read_text(encoding="utf-8"))
        if node_head(root) != "kicad_symbol_lib":
            raise ValueError(f"not a KiCad symbol library: {path}")
        index: dict[str, list] = {}
        for child in root[1:]:
            if node_head(child) == "symbol":
                index[str(child[1])] = child
        self._libraries[lib] = index
        return index

    def raw_symbol(self, lib: str, name: str) -> list:
        index = self._load(lib)
        if name not in index:
            raise KeyError(f"symbol {lib}:{name} not found")
        return index[name]

    def has_symbol(self, lib: str, name: str) -> bool:
        if lib in self._custom:
            return name in self._custom[lib]
        try:
            return name in self._load(lib)
        except (OSError, ValueError, SExprError):
            return False

    # -- flattening ---------------------------------------------------------
    def flatten(self, lib: str, name: str) -> list:
        key = (lib, name)
        if key in self._flat:
            return self._flat[key]
        if key in self._stack:
            raise ValueError(f"circular extends chain for {lib}:{name}")
        self._stack.add(key)
        try:
            node = copy.deepcopy(self.raw_symbol(lib, name))
            ext = next(
                (c for c in node[1:] if node_head(c) == "extends"),
                None,
            )
            if ext is not None:
                parent_name = str(ext[1])
                parent = copy.deepcopy(self.flatten(lib, parent_name))
                node = _merge_extends(parent, node, parent_name, name)
            self._flat[key] = node
            return node
        finally:
            self._stack.discard(key)

    def pins(self, lib: str, name: str) -> list[PinDef]:
        return symbol_pins(self.flatten(lib, name))


def _merge_extends(parent: list, child: list, parent_name: str, child_name: str) -> list:
    """Flatten an ``extends`` symbol the way KiCad does when saving a schematic.

    Parent graphics and pins are copied, sub-symbol names are renamed to the
    child, and child properties override the parent's (new child properties
    are appended).
    """
    out = copy.deepcopy(parent)
    out[1] = Q(child_name)

    child_props = [c for c in child[1:] if node_head(c) == "property"]
    child_prop_map: dict[str, list] = {}
    for prop in child_props:
        pname = prop_name(prop)
        if pname is not None:
            child_prop_map[pname] = prop

    have_props: set[str] = set()
    for i, existing in enumerate(out[1:], start=1):
        if node_head(existing) == "property":
            pname = prop_name(existing)
            if pname in child_prop_map:
                out[i] = copy.deepcopy(child_prop_map[pname])
            if pname is not None:
                have_props.add(pname)

    # Rename inherited sub-symbols: "Parent_0_1" -> "Child_0_1".
    for existing in out[1:]:
        if node_head(existing) == "symbol":
            sub_name = str(existing[1])
            prefix = parent_name + "_"
            if sub_name.startswith(prefix):
                existing[1] = Q(child_name + sub_name[len(parent_name) :])

    # Child top-level attributes replace same-named parent attributes.
    for child_node in child[1:]:
        head = node_head(child_node)
        if head in ("property", "symbol", "extends"):
            continue
        replaced = False
        for i, existing in enumerate(out[1:], start=1):
            if node_head(existing) == head:
                out[i] = copy.deepcopy(child_node)
                replaced = True
                break
        if not replaced:
            if head == "embedded_fonts":
                out.append(copy.deepcopy(child_node))
            else:
                # Keep the name first, then attributes, then properties.
                insert_at = len(out)
                for i, existing in enumerate(out[1:], start=1):
                    if node_head(existing) in ("property", "symbol",
                                               "embedded_fonts"):
                        insert_at = i
                        break
                out.insert(insert_at, copy.deepcopy(child_node))

    # Child sub-symbols (unusual for a derived symbol) override matching ones.
    for child_node in child[1:]:
        if node_head(child_node) != "symbol":
            continue
        sub = copy.deepcopy(child_node)
        sub_name = str(sub[1])
        if sub_name.startswith(parent_name + "_"):
            sub_name = child_name + sub_name[len(parent_name):]
            sub[1] = Q(sub_name)
        replaced = False
        for i, existing in enumerate(out[1:], start=1):
            if node_head(existing) == "symbol" and str(existing[1]) == sub_name:
                out[i] = sub
                replaced = True
                break
        if not replaced:
            idx = len(out)
            for i, existing in enumerate(out[1:], start=1):
                if node_head(existing) == "embedded_fonts":
                    idx = i
                    break
            out.insert(idx, sub)

    # Append child-only properties just before the sub-symbols.
    insert_at = len(out)
    for i, existing in enumerate(out[1:], start=1):
        if node_head(existing) in ("symbol", "embedded_fonts"):
            insert_at = i
            break
    for pname, prop in child_prop_map.items():
        if pname not in have_props:
            out.insert(insert_at, copy.deepcopy(prop))
            insert_at += 1

    return out


# --------------------------------------------------------------------------
# Symbol geometry / part layout
# --------------------------------------------------------------------------


def _graphics_box(node: list, box: Box) -> None:
    for child in node[1:]:
        if not isinstance(child, list):
            continue
        head = node_head(child)
        if head == "symbol":
            _graphics_box(child, box)
        elif head == "rectangle":
            start = find_child(child, "start")
            end = find_child(child, "end")
            if start and end:
                for x, y in (
                    (_f(start[1]), _f(start[2])),
                    (_f(end[1]), _f(end[2])),
                ):
                    box.add(x, -y)
        elif head in ("polyline", "bezier"):
            pts = find_child(child, "pts")
            if pts:
                for xy in children(pts, "xy"):
                    box.add(_f(xy[1]), -_f(xy[2]))
        elif head == "circle":
            center = find_child(child, "center")
            radius = find_child(child, "radius")
            if center and radius:
                cx, cy, r = _f(center[1]), _f(center[2]), _f(radius[1])
                box.add(cx - r, -(cy + r))
                box.add(cx + r, -(cy - r))
        elif head == "arc":
            for tag in ("start", "mid", "end"):
                pt = find_child(child, tag)
                if pt:
                    box.add(_f(pt[1]), -_f(pt[2]))
        elif head == "text":
            at = find_child(child, "at")
            if at:
                text = str(child[1]) if len(child) > 1 else ""
                size = 1.27
                effects = find_child(child, "effects")
                if effects:
                    font = find_child(effects, "font")
                    if font:
                        sz = find_child(font, "size")
                        if sz:
                            size = _f(sz[1])
                rot = int(_f(at[3])) if len(at) > 3 else 0
                w = _text_width(text, size)
                h = size
                if rot % 180 == 90:
                    w, h = h, w
                x, y = _f(at[1]), _f(at[2])
                box.add(x - w / 2, -(y + h / 2))
                box.add(x + w / 2, -(y - h / 2))


def _symbol_geometry_box(symbol: list, pins: list[PinDef]) -> Box:
    box = Box()
    _graphics_box(symbol, box)
    for pin in pins:
        box.add(pin.x, -pin.y)
        rad = math.radians(pin.rot)
        box.add(
            pin.x + pin.length * math.cos(rad),
            -(pin.y + pin.length * math.sin(rad)),
        )
    box.expand(1.27)
    return box


def _outward(pin_rot: int) -> tuple[int, int]:
    """Sheet-space direction pointing away from the symbol body.

    Library pins extend toward the body in their ``at`` rotation direction;
    the wire/label side is the opposite one.  Library Y is up, sheet Y is
    down, hence the sign flip on the vertical axis.
    """
    rot = int(round(pin_rot / 90.0)) * 90 % 360
    return {
        0: (-1, 0),
        90: (0, 1),
        180: (1, 0),
        270: (0, -1),
    }[rot]


def _label_rot(direction: tuple[int, int]) -> int:
    return {
        (-1, 0): 180,
        (1, 0): 0,
        (0, -1): 90,
        (0, 1): 270,
    }[direction]


@dataclass
class PinPlacement:
    pin: PinDef
    x: float  # part-local, sheet orientation (Y down)
    y: float
    direction: tuple[int, int]
    net: str | None
    nc: bool


@dataclass
class PartLayout:
    box: Box
    placements: list[PinPlacement]
    ref_pos: tuple[float, float]
    value_pos: tuple[float, float]
    ref_width: float
    value_width: float
    wires: list[tuple[float, float, float, float]]
    labels: list[tuple[float, float, int, str]]
    ncs: list[tuple[float, float]]


GRID = 1.27
STUB = 2.54
TEXT_SIZE = 1.27
# A global-label shape is twice the text size long in each direction plus an
# arrow tip (KiCad plots it 2.54 mm tall for 1.27 mm text).  Reserve that full
# drawn length, not just the text width, or references and values get placed
# on top of the label arrow.
LABEL_TIP = 1.27


def _label_extent(net: str) -> float:
    return _text_width(net) + 2 * LABEL_TIP


def _layout_part(symbol: list, pins: list[PinDef], nets: dict[str, str],
                 nc: set[str], ref: str, value: str) -> PartLayout:
    sym_box = _symbol_geometry_box(symbol, pins)
    ref_width = _text_width(ref)
    value_width = _text_width(value)

    placements: list[PinPlacement] = []
    wires: list[tuple[float, float, float, float]] = []
    labels: list[tuple[float, float, int, str]] = []
    ncs: list[tuple[float, float]] = []
    seen_wires: set[tuple[float, float, float, float]] = set()
    seen_labels: set[tuple[float, float, str]] = set()
    seen_ncs: set[tuple[float, float]] = set()

    for pin in pins:
        px = pin.x
        py = -pin.y
        net = nets.get(pin.number)
        is_nc = pin.number in nc
        direction = _outward(pin.rot)
        dx, dy = direction
        placements.append(PinPlacement(pin, px, py, direction, net, is_nc))

        if is_nc:
            key = (round(px, 4), round(py, 4))
            if key not in seen_ncs:
                seen_ncs.add(key)
                ncs.append(key)
            continue
        if net is None:
            continue
        sx = round(px + dx * STUB, 4)
        sy = round(py + dy * STUB, 4)
        wire_key = (round(px, 4), round(py, 4), sx, sy)
        if wire_key not in seen_wires:
            seen_wires.add(wire_key)
            wires.append(wire_key)
        label_key = (sx, sy, net)
        if label_key not in seen_labels:
            seen_labels.add(label_key)
            labels.append((sx, sy, _label_rot(direction), net))

    # Vertical labels own the space directly above/below their pins.  Lift the
    # Reference above them (and push the Value below them) so the visible
    # text never collides with a net label.
    label_top = math.inf
    label_bottom = -math.inf
    for _x, y, rot, net in labels:
        width = _label_extent(net)
        if rot == 90:
            label_top = min(label_top, y - width)
        elif rot == 270:
            label_bottom = max(label_bottom, y + width)

    center_x = (sym_box.xmin + sym_box.xmax) / 2.0
    ref_y = sym_box.ymin - 3.81
    if label_top < math.inf:
        ref_y = min(ref_y, label_top - 2.54)
    value_y = sym_box.ymax + 3.81
    if label_bottom > -math.inf:
        value_y = max(value_y, label_bottom + 2.54)
    ref_pos = (center_x, ref_y)
    value_pos = (center_x, value_y)

    # Vertical two-pin parts carry their text beside the body, as KiCad does,
    # so it can never sit on the labels above and below them.
    if len(labels) == 2 and all(rot in (90, 270) for _x, _y, rot, _net in labels):
        center_y = (sym_box.ymin + sym_box.ymax) / 2.0
        ref_pos = (sym_box.xmax + 1.27 + ref_width / 2.0, center_y - 1.27)
        value_pos = (sym_box.xmax + 1.27 + value_width / 2.0, center_y + 1.27)

    # A test point's value is the net it probes, so the label already shows
    # that text.  Put the pair beside the pad instead of on top of the label.
    if any(net == value for _x, _y, _rot, net in labels):
        center_y = (sym_box.ymin + sym_box.ymax) / 2.0
        ref_pos = (sym_box.xmax + 1.27 + ref_width / 2.0, center_y - 1.27)
        value_pos = (sym_box.xmax + 1.27 + value_width / 2.0, center_y + 1.27)

    box = Box(sym_box.xmin, sym_box.ymin, sym_box.xmax, sym_box.ymax)

    # Reserve room for the (visible) Reference and Value.
    box.add(ref_pos[0] - ref_width / 2.0, ref_pos[1] - TEXT_SIZE)
    box.add(ref_pos[0] + ref_width / 2.0, ref_pos[1] + TEXT_SIZE)
    box.add(value_pos[0] - value_width / 2.0, value_pos[1] - TEXT_SIZE)
    box.add(value_pos[0] + value_width / 2.0, value_pos[1] + TEXT_SIZE)

    # Reserve room for each net label, extending in the pin's outward
    # direction.  This is what guarantees that two packed parts never have
    # overlapping label text.
    for x, y, rot, net in labels:
        width = _label_extent(net)
        if rot in (0, 180):
            if rot == 0:
                box.add(x + width, y - TEXT_SIZE)
                box.add(x + width, y + TEXT_SIZE)
            else:
                box.add(x - width, y - TEXT_SIZE)
                box.add(x - width, y + TEXT_SIZE)
        else:
            if rot == 90:
                box.add(x - TEXT_SIZE, y - width)
                box.add(x + TEXT_SIZE, y - width)
            else:
                box.add(x - TEXT_SIZE, y + width)
                box.add(x + TEXT_SIZE, y + width)

    box.expand(0.635)
    return PartLayout(box, placements, ref_pos, value_pos, ref_width, value_width,
                      wires, labels, ncs)


# --------------------------------------------------------------------------
# Page geometry / shelf packing
# --------------------------------------------------------------------------


PAGE_SIZES: dict[str, tuple[float, float]] = {
    "A4": (297.0, 210.0),
    "A3": (420.0, 297.0),
    "A2": (594.0, 420.0),
    "A1": (841.0, 594.0),
    "A0": (1189.0, 841.0),
}
PAGE_GROWTH = ["A4", "A3", "A2", "A1", "A0"]

MARGIN = 12.7
SHELF_GAP = 5.08
NOTE_GAP = 5.08


def _page_order(requested: str) -> list[str]:
    requested = requested.upper()
    if requested not in PAGE_SIZES:
        raise ValueError(f"unsupported page size {requested!r}")
    idx = PAGE_GROWTH.index(requested)
    if requested in ("A0",):
        return [requested]
    # The documented growth chain ends at A1; A0 is allowed if explicitly
    # requested but is not used to auto-grow from A1.
    if requested == "A1":
        return ["A1"]
    return [requested] + [p for p in ("A3", "A2", "A1") if PAGE_GROWTH.index(p) > idx]


@dataclass
class Placed:
    origin: tuple[float, float]
    layout: PartLayout


@dataclass
class FlagPlacement:
    net: str
    x: float
    y: float


@dataclass
class SheetPacking:
    size: str
    placements: dict[str, Placed]
    flags: list[FlagPlacement]
    used_width: float
    used_bottom: float
    notes_width: float
    fits: bool


def _notes_width(notes: list[str]) -> float:
    if not notes:
        return 0.0
    widest = 0.0
    for note in notes:
        for line in str(note).splitlines() or [""]:
            widest = max(widest, _text_width(line))
    return max(50.8, widest + NOTE_GAP)


def _notes_height(notes: list[str]) -> float:
    if not notes:
        return 0.0
    y = MARGIN + 5.08
    bottom = y
    for note in notes:
        line_count = max(1, str(note).count("\n") + 1)
        bottom = max(bottom, y + line_count * 2.54)
        y += line_count * 2.54 + 2.54
    return bottom


def _snap_up(value: float) -> float:
    return math.ceil(value / GRID - 1e-9) * GRID


def _flag_width(net: str) -> float:
    return 2.54 + _text_width(net) + 3.81


def _pack_sheet(parts: list, layouts: dict[str, PartLayout], nets_per_flag: list[str],
                page: str, page_size: tuple[float, float], notes: list[str]) -> SheetPacking:
    page_w, page_h = page_size
    notes_w = _notes_width(notes)
    left = MARGIN + notes_w
    right = page_w - MARGIN
    top = MARGIN

    grouped: list = []
    group_map: dict = {}
    for index, part in enumerate(parts):
        key = part.group if part.group is not None else ("\x00single", index)
        if key not in group_map:
            group_map[key] = []
            grouped.append(group_map[key])
        group_map[key].append(part)
    ordered = [part for group in grouped for part in group]

    placements: dict[str, Placed] = {}
    cursor_x = left
    row_top = top
    row_bottom = top
    used_width = left
    used_bottom = top

    for part in ordered:
        layout = layouts[part.ref]
        box = layout.box
        if cursor_x > left and cursor_x + box.width > right:
            row_top = row_bottom + SHELF_GAP
            row_bottom = row_top
            cursor_x = left
        ox = _snap_up(cursor_x - box.xmin)
        oy = _snap_up(row_top - box.ymin)
        if ox + box.xmax > right and cursor_x > left:
            row_top = row_bottom + SHELF_GAP
            row_bottom = row_top
            cursor_x = left
            ox = _snap_up(cursor_x - box.xmin)
            oy = _snap_up(row_top - box.ymin)
        placements[part.ref] = Placed((ox, oy), layout)
        row_bottom = max(row_bottom, oy + box.ymax)
        used_width = max(used_width, ox + box.xmax)
        used_bottom = max(used_bottom, row_bottom)
        cursor_x = ox + box.xmax + SHELF_GAP

    flags: list[FlagPlacement] = []
    if nets_per_flag:
        flag_y = _snap_up(used_bottom + 7.62)
        fx = left
        for net in nets_per_flag:
            width = _flag_width(net)
            if fx > left and fx + width > right:
                fx = left
                flag_y = _snap_up(flag_y + 10.16)
            fx = _snap_up(fx)
            flags.append(FlagPlacement(net, fx, flag_y))
            fx = fx + width + 5.08
            used_width = max(used_width, fx - 5.08)
        used_bottom = max(used_bottom, flag_y + 6.35)

    fits = (
        used_width <= right + 1e-6
        and used_bottom <= page_h - MARGIN + 1e-6
        and _notes_height(notes) <= page_h - MARGIN + 1e-6
    )
    return SheetPacking(page, placements, flags, used_width, used_bottom, notes_w, fits)


def _pack_for_pages(parts: list, layouts: dict[str, PartLayout], flags: list[str],
                    requested: str, notes: list[str]) -> SheetPacking:
    last: SheetPacking | None = None
    for page in _page_order(requested):
        pack = _pack_sheet(parts, layouts, flags, page, PAGE_SIZES[page], notes)
        last = pack
        if pack.fits:
            return pack
    assert last is not None
    return last


# --------------------------------------------------------------------------
# Custom symbols
# --------------------------------------------------------------------------

ELECTRICAL_TYPES = {
    "input",
    "output",
    "bidirectional",
    "tri_state",
    "passive",
    "free",
    "unspecified",
    "power_in",
    "power_out",
    "open_collector",
    "open_emitter",
    "no_connect",
}


def _lib_property(name: str, value: str, x: float, y: float, hidden: bool,
                  size: float = 1.27) -> list:
    node = [
        "property",
        Q(name),
        Q(value),
        L("at", fmt(x), fmt(y), "0"),
        L("show_name", "no"),
        L("do_not_autoplace", "no"),
    ]
    if hidden:
        node.append(L("hide", "yes"))
    node.append(L("effects", L("font", L("size", fmt(size), fmt(size)))))
    return node


def _pin_node(number: str, name: str, etype: str, x: float, y: float, rot: int,
              length: float = 2.54) -> list:
    return L(
        "pin",
        etype,
        "line",
        L("at", fmt(x), fmt(y), str(rot)),
        L("length", fmt(length)),
        L("name", Q(name), L("effects", L("font", L("size", "1.27", "1.27")))),
        L("number", Q(number), L("effects", L("font", L("size", "1.27", "1.27")))),
    )


def _build_custom_symbol(name: str, pins: list[tuple], footprint: str,
                         description: str, reference: str) -> list:
    """Build a rectangular KiCad library symbol with 2.54 mm pin pitch."""
    sides: dict[str, list[tuple]] = {"L": [], "R": [], "T": [], "B": []}
    seen: set[str] = set()
    for entry in pins:
        if not isinstance(entry, (tuple, list)) or len(entry) != 4:
            raise ValueError(f"custom symbol {name}: each pin must be "
                             f"(number, name, electrical_type, side)")
        number, pin_name, etype, side = (str(entry[0]), str(entry[1]),
                                         str(entry[2]), str(entry[3]).upper())
        if side not in sides:
            raise ValueError(f"custom symbol {name}: pin {number} side must be L/R/T/B")
        if etype not in ELECTRICAL_TYPES:
            raise ValueError(f"custom symbol {name}: pin {number} has invalid "
                             f"electrical type {etype!r}")
        if number in seen:
            raise ValueError(f"custom symbol {name}: duplicate pin number {number}")
        seen.add(number)
        sides[side].append((number, pin_name, etype))

    # Left and right pin names grow inward from their own body edge, so the
    # width has to hold the longest left name AND the longest right name plus
    # the pin-name offset (0.508 mm per side) and a small centre gap.  Using
    # one combined maximum (the old behaviour) let U9's VOL/MICDET run into
    # SPKVDD.  The same applies to top/bottom names and the body height.
    name_gap = 2.54
    name_offset = 0.508
    lr_left = [p[1] for p in sides["L"] if p[1]]
    lr_right = [p[1] for p in sides["R"] if p[1]]
    tb_top = [p[1] for p in sides["T"] if p[1]]
    tb_bottom = [p[1] for p in sides["B"] if p[1]]
    body_w = max(10.16,
                 2 * name_offset
                 + max((_text_width(n) for n in lr_left), default=0.0)
                 + max((_text_width(n) for n in lr_right), default=0.0)
                 + name_gap,
                 (max(len(sides["T"]), len(sides["B"])) + 1) * 2.54)
    body_h = max(10.16,
                 2 * name_offset
                 + max((_text_width(n) for n in tb_top), default=0.0)
                 + max((_text_width(n) for n in tb_bottom), default=0.0)
                 + name_gap,
                 (max(len(sides["L"]), len(sides["R"])) + 1) * 2.54)
    body_w = math.ceil(body_w / 2.54 - 1e-9) * 2.54
    body_h = math.ceil(body_h / 2.54 - 1e-9) * 2.54

    pin_nodes: list = []
    for side, entries in sides.items():
        count = len(entries)
        for index, (number, pin_name, etype) in enumerate(entries):
            offset = (count - 1) * 2.54 / 2.0 - index * 2.54
            if side == "L":
                x, y, rot = -body_w / 2.0 - 2.54, offset, 0
            elif side == "R":
                x, y, rot = body_w / 2.0 + 2.54, offset, 180
            elif side == "T":
                x, y, rot = -offset, body_h / 2.0 + 2.54, 270
            else:
                x, y, rot = -offset, -body_h / 2.0 - 2.54, 90
            pin_nodes.append(_pin_node(number, pin_name, etype, x, y, rot))

    rectangle = L(
        "rectangle",
        L("start", fmt(-body_w / 2.0), fmt(body_h / 2.0)),
        L("end", fmt(body_w / 2.0), fmt(-body_h / 2.0)),
        L("stroke", L("width", "0.254"), L("type", "default")),
        L("fill", L("type", "background")),
    )

    return L(
        "symbol",
        Q(name),
        L("pin_names", L("offset", "0.508")),
        L("exclude_from_sim", "no"),
        L("in_bom", "yes"),
        L("on_board", "yes"),
        L("in_pos_files", "yes"),
        L("duplicate_pin_numbers_are_jumpers", "no"),
        _lib_property("Reference", reference, 0.0, body_h / 2.0 + 2.54, False),
        _lib_property("Value", name, 0.0, -body_h / 2.0 - 2.54, False),
        _lib_property("Footprint", footprint, 0.0, 0.0, True),
        _lib_property("Datasheet", "", 0.0, 0.0, True),
        _lib_property("Description", description, 0.0, 0.0, True),
        L("symbol", Q(name + "_0_1"), rectangle),
        L("symbol", Q(name + "_1_1"), *pin_nodes),
        L("embedded_fonts", "no"),
    )


# --------------------------------------------------------------------------
# Public data model
# --------------------------------------------------------------------------


def _split_lib_id(lib_id: str) -> tuple[str, str]:
    text = str(lib_id)
    if ":" not in text:
        raise ValueError(f"symbol must be 'Lib:Name', got {lib_id!r}")
    lib, name = text.split(":", 1)
    if not lib or not name:
        raise ValueError(f"symbol must be 'Lib:Name', got {lib_id!r}")
    return lib, name


def _unique_pin_numbers(pins: list[PinDef]) -> list[str]:
    numbers: list[str] = []
    seen: set[str] = set()
    for pin in pins:
        if pin.number not in seen:
            seen.add(pin.number)
            numbers.append(pin.number)
    return numbers


@dataclass
class PartDef:
    sheet_name: str
    ref: str
    sym: str
    value: str
    fp: str
    nets: dict[str, str]
    nc: tuple[str, ...]
    mpn: str
    mfr: str
    dnp: bool
    desc: str
    fields: dict[str, str]
    group: str | None
    pins: list[PinDef]
    symbol: list
    datasheet: str
    description: str

    @property
    def pin_numbers(self) -> list[str]:
        return _unique_pin_numbers(self.pins)


_RESERVED_FIELDS = {
    "Reference", "Value", "Footprint", "Datasheet", "Description",
    "MPN", "Manufacturer",
}


class Sheet:
    """A hierarchical child sheet of a generated root sheet."""

    def __init__(self, project: "Project", name: str, filename: str, page_size: str):
        self.project = project
        self.name = str(name)
        self.filename = filename
        self.page_size = str(page_size).upper()
        self.parts: list[PartDef] = []
        self.notes: list[str] = []
        self.pwr_flags: list[str] = []
        self.uuid = make_uuid("sheet", project.name, self.name)

    def part(self, ref, sym, value, fp, nets, mpn="", mfr="", dnp=False, nc=(),
             desc="", fields=None, group=None) -> None:
        self.project._add_part(self, ref, sym, value, fp, nets, mpn, mfr, dnp,
                               nc, desc, fields, group)

    def note(self, text) -> None:
        text = str(text)
        if text not in self.notes:
            self.notes.append(text)

    def pwr_flag(self, net) -> None:
        net = str(net)
        if net not in self.pwr_flags:
            self.pwr_flags.append(net)


class Project:
    """Hierarchical KiCad 10 project generator."""

    def __init__(self, name, outdir, title, rev, company="", date=""):
        self.name = str(name)
        if not self.name or any(ch in self.name for ch in '\\/:*?"<>|'):
            raise ValueError(f"invalid project name {name!r}")
        self.outdir = Path(outdir)
        self.title = str(title)
        self.rev = str(rev)
        self.company = str(company)
        self.date = str(date)
        self.sheets: list[Sheet] = []
        self._parts: list[PartDef] = []
        self._refs: set[str] = set()
        self._resolver = LibraryResolver()
        self._custom_names: list[str] = []
        self._root_uuid = make_uuid("root", self.name)

    # -- construction -------------------------------------------------------
    def sheet(self, name, filename, page_size="A3") -> Sheet:
        name = str(name)
        filename = str(filename).replace("\\", "/")
        if not filename.endswith(".kicad_sch"):
            filename += ".kicad_sch"
        for existing in self.sheets:
            if existing.name == name:
                if filename != existing.filename:
                    raise ValueError(f"sheet {name!r} already exists with a "
                                     f"different filename")
                return existing
        candidate = Path(filename)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise ValueError(f"sheet filename must be relative to the output dir: {filename}")
        for existing in self.sheets:
            if existing.filename == filename:
                raise ValueError(f"duplicate sheet filename {filename!r}")
        page = str(page_size).upper()
        if page not in PAGE_SIZES:
            raise ValueError(f"unsupported page size {page_size!r}")
        sheet = Sheet(self, name, filename, page)
        self.sheets.append(sheet)
        return sheet

    def custom_symbol(self, name, pins, footprint="", description="", reference="U") -> None:
        name = str(name)
        if name.startswith("PiPod:"):
            name = name.split(":", 1)[1]
        if not name:
            raise ValueError("custom symbol name must not be empty")
        if name in self._custom_names:
            raise ValueError(f"duplicate custom symbol {name!r}")
        node = _build_custom_symbol(name, pins, str(footprint), str(description),
                                    str(reference))
        self._resolver.add_custom_symbol("PiPod", name, node)
        self._custom_names.append(name)

    def symbol_pins(self, sym) -> list[tuple[str, str, str]]:
        """Return (number, name, electrical_type) for every pin of ``sym``."""
        lib, name = _split_lib_id(sym)
        return [(p.number, p.name, p.etype) for p in self._resolver.pins(lib, name)]

    def iter_parts(self):
        yield from self._parts

    @property
    def root_schematic_path(self) -> Path:
        return self.outdir / f"{self.name}.kicad_sch"

    # -- part validation ----------------------------------------------------
    def _add_part(self, sheet: Sheet, ref, sym, value, fp, nets, mpn, mfr, dnp,
                  nc, desc, fields, group) -> None:
        ref = str(ref)
        if not ref:
            raise ValueError("part reference must not be empty")
        if ref in self._refs:
            raise ValueError(f"duplicate reference {ref}")
        lib, name = _split_lib_id(sym)
        try:
            pins = self._resolver.pins(lib, name)
        except (FileNotFoundError, KeyError) as exc:
            raise ValueError(f"part {ref}: cannot load symbol {sym}: {exc}") from None
        if not pins:
            raise ValueError(f"part {ref}: symbol {sym} has no pins")

        net_map: dict[str, str] = {}
        if not isinstance(nets, dict):
            raise ValueError(f"part {ref}: nets must be a dict of pin -> net")
        for key, net_name in nets.items():
            net_map[str(key)] = str(net_name)
        if isinstance(nc, str):
            nc_values = (nc,)
        else:
            nc_values = tuple(str(item) for item in nc)
        nc_set = set(nc_values)

        pin_numbers = set(_unique_pin_numbers(pins))
        unknown = (set(net_map) | nc_set) - pin_numbers
        if unknown:
            bad = sorted(unknown)[0]
            raise ValueError(f"part {ref}: pin {bad} does not exist on {sym}")
        for number in _unique_pin_numbers(pins):
            in_nets = number in net_map
            in_nc = number in nc_set
            if in_nets == in_nc:
                raise ValueError(
                    f"part {ref}: pin {number} must appear in exactly one of "
                    f"nets or nc"
                )

        fdict = dict(fields or {})
        bad_fields = _RESERVED_FIELDS.intersection(fdict)
        if bad_fields:
            raise ValueError(f"part {ref}: fields may not override "
                             f"{sorted(bad_fields)[0]}")
        fp_text = str(fp)
        if fp_text and ":" not in fp_text:
            raise ValueError(f"part {ref}: footprint must be 'Lib:Footprint', "
                             f"got {fp_text!r}")

        symbol = copy.deepcopy(self._resolver.flatten(lib, name))
        datasheet = property_value(symbol, "Datasheet")
        symbol_desc = property_value(symbol, "Description")
        description = str(desc) if str(desc) else symbol_desc

        part = PartDef(
            sheet_name=sheet.name,
            ref=ref,
            sym=f"{lib}:{name}",
            value=str(value),
            fp=fp_text,
            nets=net_map,
            nc=tuple(nc_values),
            mpn=str(mpn),
            mfr=str(mfr),
            dnp=bool(dnp),
            desc=str(desc),
            fields={str(k): str(v) for k, v in fdict.items()},
            group=None if group is None else str(group),
            pins=pins,
            symbol=symbol,
            datasheet=datasheet,
            description=description,
        )
        sheet.parts.append(part)
        self._parts.append(part)
        self._refs.add(ref)

    # -- output -------------------------------------------------------------
    def write(self):
        out = self.outdir
        (out / "lib").mkdir(parents=True, exist_ok=True)
        (out / "lib" / "PiPod.pretty").mkdir(parents=True, exist_ok=True)

        for sheet in self.sheets:
            pack, layouts = self._sheet_pack(sheet)
            document = self._child_document(sheet, pack, layouts)
            self._write_text(out / sheet.filename, emit_document(document))
        root_document = self._root_document()
        self._write_text(self.root_schematic_path, emit_document(root_document))
        self._write_text(out / "lib" / "PiPod.kicad_sym", emit_document(self._custom_library()))
        self._write_text(out / "sym-lib-table", emit_document(self._sym_lib_table()))
        self._write_text(out / "fp-lib-table", emit_document(self._fp_lib_table()))
        self._write_text(out / f"{self.name}.kicad_pro",
                         json.dumps(self._project_json(), indent=2, sort_keys=True) + "\n")

    @staticmethod
    def _write_text(path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)

    def _sheet_pack(self, sheet: Sheet):
        layouts = {}
        for part in sheet.parts:
            layouts[part.ref] = _layout_part(
                part.symbol, part.pins, part.nets, set(part.nc), part.ref, part.value
            )
        pack = _pack_for_pages(sheet.parts, layouts, sheet.pwr_flags,
                               sheet.page_size, sheet.notes)
        return pack, layouts

    # -- documents ----------------------------------------------------------
    def _title_block(self) -> list:
        return L(
            "title_block",
            L("title", Q(self.title)),
            L("date", Q(self.date)),
            L("rev", Q(self.rev)),
            L("company", Q(self.company)),
        )

    def _embedded_symbol(self, lib_id: str) -> list:
        lib, name = _split_lib_id(lib_id)
        node = copy.deepcopy(self._resolver.flatten(lib, name))
        node[1] = Q(f"{lib}:{name}")
        return node

    def _lib_symbols_node(self, lib_ids) -> list:
        unique = sorted(set(lib_ids))
        if not unique:
            return L("lib_symbols")
        return L("lib_symbols", *[self._embedded_symbol(lib_id) for lib_id in unique])

    def _child_document(self, sheet: Sheet, pack: SheetPacking,
                        layouts: dict[str, PartLayout]) -> list:
        elements: list = []
        used_symbols: set[str] = set()

        # Every net label is a global label.  Global shapes are centred on
        # their wire and exactly 2.54 mm wide/tall, so neighbouring labels at
        # the 2.54 mm pin pitch tile instead of colliding; KiCad's local
        # rotated labels sit on one side of the wire and run into the box of
        # the pin above.  Net names are unique by construction, so global
        # scope does not merge unrelated nets.
        # Power flags: symbol pin -> 2.54 mm wire -> label.
        sheet_number = self.sheets.index(sheet) + 1
        for index, flag in enumerate(pack.flags, start=1):
            x, y = flag.x, flag.y
            ref = f"#FLG{sheet_number:02d}{index:02d}"
            elements.append(self._wire_node(x, y, x + STUB, y,
                                            make_uuid("wire", self.name, sheet.name,
                                                      "pwr", flag.net)))
            elements.append(self._label_node(
                flag.net, x + STUB, y, 0,
                make_uuid("label", self.name, sheet.name, "pwr", flag.net),
                global_net=True))
            elements.append(self._power_flag_instance(sheet, ref, x, y))
            used_symbols.add("power:PWR_FLAG")

        for part in sheet.parts:
            placed = pack.placements[part.ref]
            ox, oy = placed.origin
            layout = layouts[part.ref]
            used_symbols.add(part.sym)
            for index, (x1, y1, x2, y2) in enumerate(layout.wires):
                elements.append(self._wire_node(
                    ox + x1, oy + y1, ox + x2, oy + y2,
                    make_uuid("wire", self.name, sheet.name, part.ref, index)))
            for index, (x, y, rot, net) in enumerate(layout.labels):
                elements.append(self._label_node(
                    net, ox + x, oy + y, rot,
                    make_uuid("label", self.name, sheet.name, part.ref, index),
                    global_net=True))
            for index, (x, y) in enumerate(layout.ncs):
                elements.append(self._no_connect_node(
                    ox + x, oy + y,
                    make_uuid("nc", self.name, sheet.name, part.ref, index)))
            elements.append(self._part_instance(sheet, part, ox, oy,
                                               layout.ref_pos, layout.value_pos))

        note_y = MARGIN + 5.08
        for index, note in enumerate(sheet.notes):
            elements.append(self._note_node(
                note, MARGIN, note_y,
                make_uuid("note", self.name, sheet.name, index)))
            line_count = max(1, str(note).count("\n") + 1)
            note_y += line_count * 2.54 + 2.54

        return L(
            "kicad_sch",
            L("version", KICAD_VERSION),
            L("generator", Q("kicadgen")),
            L("generator_version", Q(KICAD_GENERATOR_VERSION)),
            L("uuid", Q(sheet.uuid)),
            L("paper", Q(pack.size)),
            self._title_block(),
            self._lib_symbols_node(used_symbols),
            *elements,
        )

    def _root_document(self) -> list:
        max_width = 50.8
        for sheet in self.sheets:
            max_width = max(max_width, _text_width(sheet.name) + 10.16,
                            _text_width(sheet.filename) + 10.16)
        sheet_w = math.ceil(max_width / 2.54 - 1e-9) * 2.54
        sheet_h = 25.4
        gap = 12.7
        # The drawing frame's inner border sits at 12 mm.  The sheet symbols
        # and their name text start at 12.7 mm with MARGIN, which reads as
        # touching the frame, so give the root sheet an extra 2.54 mm.
        root_margin = MARGIN + 2.54

        chosen = "A3"
        positions: list[tuple[float, float]] = []
        for page in ("A3", "A2", "A1"):
            page_w, page_h = PAGE_SIZES[page]
            cols = max(1, int((page_w - 2 * root_margin + gap) // (sheet_w + gap)))
            rows = math.ceil(max(1, len(self.sheets)) / cols)
            needed = root_margin * 2 + rows * sheet_h + max(0, rows - 1) * gap
            if needed <= page_h or page == "A1":
                chosen = page
                positions = []
                for index in range(len(self.sheets)):
                    row, col = divmod(index, cols)
                    positions.append((root_margin + col * (sheet_w + gap),
                                      root_margin + 7.62 + row * (sheet_h + gap)))
                break

        sheets_nodes = []
        for index, sheet in enumerate(self.sheets):
            x, y = positions[index]
            sheets_nodes.append(self._sheet_symbol(sheet, index + 2, x, y,
                                                   sheet_w, sheet_h))
        return L(
            "kicad_sch",
            L("version", KICAD_VERSION),
            L("generator", Q("kicadgen")),
            L("generator_version", Q(KICAD_GENERATOR_VERSION)),
            L("uuid", Q(self._root_uuid)),
            L("paper", Q(chosen)),
            self._title_block(),
            L("lib_symbols"),
            *sheets_nodes,
            L("sheet_instances", L("path", Q("/"), L("page", Q("1")))),
            L("embedded_fonts", "no"),
        )

    # -- element builders ---------------------------------------------------
    def _wire_node(self, x1, y1, x2, y2, node_uuid) -> list:
        return L(
            "wire",
            L("pts",
              L("xy", fmt(x1), fmt(y1)),
              L("xy", fmt(x2), fmt(y2))),
            L("stroke", L("width", "0"), L("type", "default")),
            L("uuid", Q(node_uuid)),
        )

    def _label_node(self, net: str, x, y, rot: int, node_uuid: str,
                    global_net: bool = False) -> list:
        justify = ["left", "bottom"] if rot in (0, 90) else ["right", "bottom"]
        if not global_net:
            return L(
                "label",
                Q(net),
                L("at", fmt(x), fmt(y), str(rot)),
                L("effects",
                  L("font", L("size", "1.27", "1.27")),
                  L("justify", *justify)),
                L("uuid", Q(node_uuid)),
            )
        side = ["left"] if rot in (0, 90) else ["right"]
        return L(
            "global_label",
            Q(net),
            L("shape", "bidirectional"),
            L("at", fmt(x), fmt(y), str(rot)),
            L("fields_autoplaced", "yes"),
            L("effects",
              L("font", L("size", "1.27", "1.27")),
              L("justify", *side)),
            L("uuid", Q(node_uuid)),
            L("property", Q("Intersheetrefs"), Q("${INTERSHEET_REFS}"),
              L("at", fmt(x), fmt(y), "0"),
              L("effects",
                L("font", L("size", "1.27", "1.27")),
                L("justify", *side),
                L("hide", "yes"))),
        )

    def _no_connect_node(self, x, y, node_uuid: str) -> list:
        return L(
            "no_connect",
            L("at", fmt(x), fmt(y)),
            L("uuid", Q(node_uuid)),
        )

    def _note_node(self, text: str, x, y, node_uuid: str) -> list:
        return L(
            "text",
            Q(text),
            L("exclude_from_sim", "no"),
            L("at", fmt(x), fmt(y), "0"),
            L("effects",
              L("font", L("size", "1.27", "1.27")),
              L("justify", "left", "top")),
            L("uuid", Q(node_uuid)),
        )

    def _instance_property(self, name: str, value: str, x, y, hidden: bool) -> list:
        node = [
            "property",
            Q(name),
            Q(value),
            L("at", fmt(x), fmt(y), "0"),
        ]
        if hidden:
            node.append(L("hide", "yes"))
        node += [
            L("show_name", "no"),
            L("do_not_autoplace", "no"),
            L("effects", L("font", L("size", "1.27", "1.27"))),
        ]
        return node

    def _part_instance(self, sheet: Sheet, part: PartDef, ox, oy,
                       ref_pos, value_pos) -> list:
        path = f"/{self._root_uuid}/{sheet.uuid}"
        rx = ox + ref_pos[0]
        ry = oy + ref_pos[1]
        vx = ox + value_pos[0]
        vy = oy + value_pos[1]
        properties = [
            self._instance_property("Reference", part.ref, rx, ry, False),
            self._instance_property("Value", part.value, vx, vy, False),
            self._instance_property("Footprint", part.fp, ox, oy, True),
            self._instance_property("Datasheet", part.datasheet, ox, oy, True),
            self._instance_property("Description", part.description, ox, oy, True),
            self._instance_property("MPN", part.mpn, ox, oy, True),
            self._instance_property("Manufacturer", part.mfr, ox, oy, True),
        ]
        for field_name, field_value in part.fields.items():
            properties.append(
                self._instance_property(field_name, field_value, ox, oy, True)
            )
        pin_nodes = []
        for number in part.pin_numbers:
            pin_nodes.append(L("pin", Q(number),
                               L("uuid", Q(make_uuid("pin", self.name,
                                                     sheet.name, part.ref, number)))))
        return L(
            "symbol",
            L("lib_id", Q(part.sym)),
            L("at", fmt(ox), fmt(oy), "0"),
            L("unit", "1"),
            L("body_style", "1"),
            L("exclude_from_sim", "no"),
            L("in_bom", "yes"),
            L("on_board", "yes"),
            L("in_pos_files", "yes"),
            L("dnp", "yes" if part.dnp else "no"),
            L("uuid", Q(make_uuid("part", self.name, sheet.name, part.ref))),
            *properties,
            *pin_nodes,
            L("instances",
              L("project", Q(self.name),
                L("path", Q(path),
                  L("reference", Q(part.ref)),
                  L("unit", "1")))),
        )

    def _power_flag_instance(self, sheet: Sheet, ref: str, x, y) -> list:
        path = f"/{self._root_uuid}/{sheet.uuid}"
        properties = [
            self._instance_property("Reference", ref, x, y - 1.905, True),
            self._instance_property("Value", "PWR_FLAG", x, y - 3.81, False),
            self._instance_property("Footprint", "", x, y, True),
            self._instance_property("Datasheet", "", x, y, True),
            self._instance_property(
                "Description",
                "Special symbol for telling ERC where power comes from",
                x, y, True),
        ]
        return L(
            "symbol",
            L("lib_id", Q("power:PWR_FLAG")),
            L("at", fmt(x), fmt(y), "0"),
            L("unit", "1"),
            L("body_style", "1"),
            L("exclude_from_sim", "no"),
            L("in_bom", "yes"),
            L("on_board", "yes"),
            L("in_pos_files", "yes"),
            L("dnp", "no"),
            L("uuid", Q(make_uuid("pwrflag", self.name, sheet.name, ref))),
            *properties,
            L("pin", Q("1"),
              L("uuid", Q(make_uuid("pwrflagpin", self.name, sheet.name, ref)))),
            L("instances",
              L("project", Q(self.name),
                L("path", Q(path),
                  L("reference", Q(ref)),
                  L("unit", "1")))),
        )

    def _sheet_symbol(self, sheet: Sheet, page: int, x, y, width, height) -> list:
        return L(
            "sheet",
            L("at", fmt(x), fmt(y)),
            L("size", fmt(width), fmt(height)),
            L("exclude_from_sim", "no"),
            L("in_bom", "yes"),
            L("on_board", "yes"),
            L("dnp", "no"),
            L("stroke", L("width", "0"), L("type", "solid")),
            L("fill", L("color", "0", "0", "0", "0")),
            L("uuid", Q(make_uuid("sheetsym", self.name, sheet.name))),
            L("property", Q("Sheetname"), Q(sheet.name),
              L("at", fmt(x), fmt(y - 0.5), "0"),
              L("show_name", "no"),
              L("do_not_autoplace", "no"),
              L("effects",
                L("font", L("size", "1.27", "1.27")),
                L("justify", "left", "bottom"))),
            L("property", Q("Sheetfile"), Q(sheet.filename),
              L("at", fmt(x), fmt(y + height + 1.27), "0"),
              L("show_name", "no"),
              L("do_not_autoplace", "no"),
              L("effects",
                L("font", L("size", "1.27", "1.27")),
                L("justify", "left", "top"))),
            L("instances",
              L("project", Q(self.name),
                L("path", Q(f"/{self._root_uuid}"),
                  L("page", Q(str(page)))))),
        )

    # -- library tables and project file -----------------------------------
    def _used_symbol_libraries(self) -> list[str]:
        libs = {"PiPod"}
        for part in self._parts:
            lib, _ = _split_lib_id(part.sym)
            libs.add(lib)
        for sheet in self.sheets:
            if sheet.pwr_flags:
                libs.add("power")
        return sorted(libs)

    def _used_footprint_libraries(self) -> list[str]:
        libs = {"PiPod"}
        for part in self._parts:
            if part.fp and ":" in part.fp:
                libs.add(part.fp.split(":", 1)[0])
        return sorted(libs)

    def _sym_lib_table(self) -> list:
        entries = []
        for lib in self._used_symbol_libraries():
            if lib == "PiPod":
                uri = "${KIPRJMOD}/lib/PiPod.kicad_sym"
            else:
                uri = "${KICAD10_SYMBOL_DIR}/" + lib + ".kicad_sym"
            entries.append(L(
                "lib",
                L("name", Q(lib)),
                L("type", Q("KiCad")),
                L("uri", Q(uri)),
                L("options", Q("")),
                L("descr", Q("")),
            ))
        return L("sym_lib_table", L("version", "7"), *entries)

    def _fp_lib_table(self) -> list:
        entries = []
        for lib in self._used_footprint_libraries():
            if lib == "PiPod":
                uri = "${KIPRJMOD}/lib/PiPod.pretty"
            else:
                uri = "${KICAD10_FOOTPRINT_DIR}/" + lib + ".pretty"
            entries.append(L(
                "lib",
                L("name", Q(lib)),
                L("type", Q("KiCad")),
                L("uri", Q(uri)),
                L("options", Q("")),
                L("descr", Q("")),
            ))
        return L("fp_lib_table", L("version", "7"), *entries)

    def _custom_library(self) -> list:
        symbols = []
        for name in sorted(self._custom_names):
            node = self._resolver.flatten("PiPod", name)
            symbols.append(copy.deepcopy(node))
        return L(
            "kicad_symbol_lib",
            L("version", KICAD_SYMBOL_LIB_VERSION),
            L("generator", Q("kicad_symbol_editor")),
            L("generator_version", Q(KICAD_GENERATOR_VERSION)),
            *symbols,
        )

    def _project_json(self) -> dict:
        default_class = {
            "bus_width": 12,
            "clearance": 0.2,
            "diff_pair_gap": 0.25,
            "diff_pair_via_gap": 0.25,
            "diff_pair_width": 0.2,
            "line_style": 0,
            "microvia_diameter": 0.3,
            "microvia_drill": 0.1,
            "name": "Default",
            "pcb_color": "rgba(0, 0, 0, 0.000)",
            "priority": 2147483647,
            "schematic_color": "rgba(0, 0, 0, 0.000)",
            "track_width": 0.2,
            "tuning_profile": "",
            "via_diameter": 0.6,
            "via_drill": 0.3,
            "wire_width": 6,
        }
        return {
            "board": {
                "3dviewports": [],
                "ipc2581": {
                    "bom_rev": "", "dist": "", "distpn": "", "internal_id": "",
                    "mfg": "", "mpn": "", "sch_revision": "",
                },
                "layer_pairs": [],
                "layer_presets": [],
                "viewports": [],
            },
            "boards": [],
            "component_class_settings": {
                "assignments": [],
                "meta": {"version": 0},
                "sheet_component_classes": {"enabled": False},
            },
            "cvpcb": {"equivalence_files": []},
            "erc": {
                "erc_exclusions": [],
                "meta": {"version": 0},
                "rule_severities": {
                    "bus_definition_conflict": "error",
                    "bus_entry_needed": "error",
                    "bus_to_bus_conflict": "error",
                    "bus_to_net_conflict": "error",
                    "different_unit_footprint": "error",
                    "different_unit_net": "error",
                    "duplicate_reference": "error",
                    "duplicate_sheet_names": "error",
                    "endpoint_off_grid": "warning",
                    "extra_units": "error",
                    "field_name_whitespace": "warning",
                    "footprint_filter": "ignore",
                    "footprint_link_issues": "warning",
                    "four_way_junction": "ignore",
                    "ground_pin_not_ground": "warning",
                    "hier_label_mismatch": "error",
                    "isolated_pin_label": "warning",
                    "label_dangling": "error",
                    "label_multiple_wires": "warning",
                    "lib_symbol_issues": "warning",
                    "lib_symbol_mismatch": "warning",
                    "missing_bidi_pin": "warning",
                    "missing_input_pin": "warning",
                    "missing_power_pin": "error",
                    "missing_unit": "warning",
                    "multiple_net_names": "warning",
                    "net_not_bus_member": "warning",
                    "no_connect_connected": "warning",
                    "no_connect_dangling": "warning",
                    "pin_not_connected": "error",
                    "pin_not_driven": "error",
                    "pin_to_pin": "warning",
                    "power_pin_not_driven": "error",
                    "same_local_global_label": "warning",
                    "similar_label_and_power": "warning",
                    "similar_labels": "warning",
                    "similar_power": "warning",
                    "simulation_model_issue": "ignore",
                    "single_global_label": "ignore",
                    "stacked_pin_name": "warning",
                    "unannotated": "error",
                    "unconnected_wire_endpoint": "warning",
                    "undefined_netclass": "error",
                    "unit_value_mismatch": "error",
                    "unresolved_variable": "error",
                    "wire_dangling": "error",
                },
            },
            "libraries": {
                "pinned_footprint_libs": [],
                "pinned_symbol_libs": [],
            },
            "meta": {"filename": f"{self.name}.kicad_pro", "version": 3},
            "net_settings": {
                "classes": [default_class],
                "meta": {"version": 5},
                "net_colors": None,
                "netclass_assignments": None,
                "netclass_patterns": [],
            },
            "pcbnew": {
                "last_paths": {
                    "idf": "", "netlist": "", "plot": "", "specctra_dsn": "",
                    "vrml": "",
                },
                "page_layout_descr_file": "",
            },
            "schematic": {
                "annotate_start_num": 0,
                "annotation": {"method": 0, "sort_order": 0},
                "bom_export_filename": "${PROJECTNAME}.csv",
                "bom_fmt_presets": [],
                "bom_fmt_settings": {
                    "field_delimiter": ",",
                    "keep_line_breaks": False,
                    "keep_tabs": False,
                    "name": "CSV",
                    "ref_delimiter": ",",
                    "ref_range_delimiter": "",
                    "string_delimiter": '"',
                },
                "bom_presets": [],
                "bom_settings": {
                    "exclude_dnp": False,
                    "fields_ordered": [
                        {"group_by": False, "label": "Reference",
                         "name": "Reference", "show": True},
                        {"group_by": False, "label": "Qty",
                         "name": "${QUANTITY}", "show": True},
                        {"group_by": True, "label": "Value",
                         "name": "Value", "show": True},
                        {"group_by": True, "label": "DNP",
                         "name": "${DNP}", "show": True},
                        {"group_by": True, "label": "Footprint",
                         "name": "Footprint", "show": True},
                        {"group_by": False, "label": "Datasheet",
                         "name": "Datasheet", "show": True},
                    ],
                    "filter_string": "",
                    "group_symbols": True,
                    "include_excluded_from_bom": True,
                    "name": "Default Editing",
                    "sort_asc": True,
                    "sort_field": "Reference",
                },
                "bus_aliases": {},
                "connection_grid_size": 50.0,
                "drawing": {
                    "dashed_lines_dash_length_ratio": 12.0,
                    "dashed_lines_gap_length_ratio": 3.0,
                    "default_line_thickness": 6.0,
                    "default_text_size": 50.0,
                    "field_names": [],
                    "hop_over_size_choice": 0,
                    "intersheets_ref_own_page": False,
                    "intersheets_ref_prefix": "",
                    "intersheets_ref_short": False,
                    "intersheets_ref_show": False,
                    "intersheets_ref_suffix": "",
                    "junction_size_choice": 3,
                    "label_size_ratio": 0.375,
                    "operating_point_overlay_i_precision": 3,
                    "operating_point_overlay_i_range": "~A",
                    "operating_point_overlay_v_precision": 3,
                    "operating_point_overlay_v_range": "~V",
                    "overbar_offset_ratio": 1.23,
                    "pin_symbol_size": 25.0,
                    "text_offset_ratio": 0.15,
                },
                "legacy_lib_dir": "",
                "legacy_lib_list": [],
                "meta": {"version": 1},
                "page_layout_descr_file": "",
                "plot_directory": "",
                "reuse_designators": True,
                "subpart_first_id": 65,
                "subpart_id_separator": 0,
                "top_level_sheets": [
                    {
                        "filename": f"{self.name}.kicad_sch",
                        "name": "Root",
                        "uuid": self._root_uuid,
                    }
                ],
                "variants": [],
            },
            "sheets": [
                [self._root_uuid, "Root"],
                *[[sheet.uuid, sheet.name] for sheet in self.sheets],
            ],
            "text_variables": {},
            "tuning_profiles": {
                "meta": {"version": 0},
                "tuning_profiles_impedance_geometric": [],
            },
        }

    # -- BOM ----------------------------------------------------------------
    def export_bom(self, path) -> None:
        path = Path(path)
        extra_fields: list[str] = []
        for part in self._parts:
            for name in part.fields:
                if name not in extra_fields:
                    extra_fields.append(name)
        header = [
            "Reference", "Value", "Qty", "Manufacturer", "MPN", "Footprint",
            "Symbol", "DNP", "Sheet", "Description", *extra_fields,
        ]
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerow(header)
            for part in self._parts:
                writer.writerow([
                    part.ref,
                    part.value,
                    1,
                    part.mfr,
                    part.mpn,
                    part.fp,
                    part.sym,
                    "yes" if part.dnp else "no",
                    part.sheet_name,
                    part.description,
                    *[part.fields.get(name, "") for name in extra_fields],
                ])
