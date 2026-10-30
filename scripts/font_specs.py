"""Single source of truth for the generated Inter font instances.

Imported by scripts/regen_assets.py (generation) and
scripts/gen_font_preview.py (human-readable proof sheet).
"""

from __future__ import annotations

from typing import List, TypedDict


class FontSpec(TypedDict):
    name: str
    size: int
    weight: int
    note: str


FONT_TTF = "assets/fonts/Inter-Variable.ttf"
FONT_LICENSE = "SIL Open Font License 1.1 (OFL-1.1)"

FONT_SPECS: List[FontSpec] = [
    {"name": "font_inter_12", "size": 12, "weight": 400, "note": "body small / captions"},
    {"name": "font_inter_14", "size": 14, "weight": 400, "note": "body"},
    {"name": "font_inter_16sb", "size": 16, "weight": 600, "note": "list item selected"},
    {"name": "font_inter_20b", "size": 20, "weight": 700, "note": "screen title"},
    {"name": "font_inter_28b", "size": 28, "weight": 700, "note": "now-playing title"},
]
