#!/usr/bin/env python3
"""Semantic ownership test for the zed two-deck FX top bars.

This expands the skin's Template graph from overview.xml using only Python's
standard library. It deliberately tests Mixxx's native EffectSelector
rack/unit/slot identity instead of looking for a loaded_effect ConfigKey:
WEffectSelector attaches to EffectSlot directly, and EffectSlot later confirms
the 1-indexed loaded_effect Control Object.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SKIN = ROOT / "skin" / "zed"
ENTRY = SKIN / "overview.xml"


@dataclass(frozen=True)
class Selector:
    rack: int
    unit: int
    slot: int
    source: Path


class SkinExpansion:
    def __init__(self) -> None:
        self._cache: dict[Path, ET.Element] = {}
        self.selectors: list[Selector] = []
        self.config_keys: list[tuple[str, Path]] = []

    def parse(self, path: Path) -> ET.Element:
        path = path.resolve()
        if path not in self._cache:
            if not path.is_file():
                raise AssertionError(f"skin template does not exist: {path}")
            try:
                self._cache[path] = ET.parse(path).getroot()
            except ET.ParseError as error:
                raise AssertionError(f"invalid XML in {path}: {error}") from error
        return self._cache[path]

    @staticmethod
    def resolve_text(node: ET.Element, variables: dict[str, str]) -> str:
        parts: list[str] = [node.text or ""]
        for child in node:
            if child.tag == "Variable":
                name = child.attrib.get("name", "")
                if name not in variables:
                    parts.append(f"<UNRESOLVED:{name}>")
                else:
                    parts.append(variables[name])
            else:
                parts.append(SkinExpansion.resolve_text(child, variables))
            parts.append(child.tail or "")
        return "".join(parts).strip()

    @staticmethod
    def direct_child(node: ET.Element, name: str) -> ET.Element:
        child = node.find(name)
        if child is None:
            raise AssertionError(f"EffectSelector is missing <{name}>")
        return child

    @staticmethod
    def integer(text: str, label: str, source: Path) -> int:
        if not re.fullmatch(r"[0-9]+", text):
            raise AssertionError(
                f"{source}: unresolved or non-numeric {label}: {text!r}"
            )
        return int(text)

    def expand(self, path: Path, variables: dict[str, str] | None = None) -> None:
        self._walk(self.parse(path), path.resolve(), dict(variables or {}), 0)

    def _walk(
        self,
        node: ET.Element,
        source: Path,
        variables: dict[str, str],
        depth: int,
    ) -> None:
        if depth > 32:
            raise AssertionError(f"template recursion is too deep at {source}")

        template_src = node.attrib.get("src") if node.tag == "Template" else None
        if template_src:
            child_variables = dict(variables)
            for setter in node.findall("SetVariable"):
                name = setter.attrib.get("name")
                if not name:
                    raise AssertionError(f"{source}: SetVariable has no name")
                child_variables[name] = self.resolve_text(setter, child_variables)

            if template_src.startswith("skin:"):
                template_path = SKIN / template_src[len("skin:") :]
            else:
                template_path = source.parent / template_src
            self._walk(
                self.parse(template_path),
                template_path.resolve(),
                child_variables,
                depth + 1,
            )
            return

        if node.tag == "EffectSelector":
            rack_text = self.resolve_text(
                self.direct_child(node, "EffectRack"), variables
            )
            unit_text = self.resolve_text(
                self.direct_child(node, "EffectUnit"), variables
            )
            slot_text = self.resolve_text(
                self.direct_child(node, "Effect"), variables
            )
            self.selectors.append(
                Selector(
                    rack=self.integer(rack_text, "EffectRack", source),
                    unit=self.integer(unit_text, "EffectUnit", source),
                    slot=self.integer(slot_text, "Effect", source),
                    source=source,
                )
            )

        if node.tag == "ConfigKey":
            resolved = self.resolve_text(node, variables)
            if "<UNRESOLVED:" in resolved:
                raise AssertionError(f"{source}: unresolved ConfigKey {resolved!r}")
            self.config_keys.append((resolved, source))

        for child in node:
            if child.tag != "SetVariable":
                self._walk(child, source, variables, depth)


def require_key(expansion: SkinExpansion, expected: str) -> None:
    matches = [source for value, source in expansion.config_keys if value == expected]
    if len(matches) != 1:
        locations = ", ".join(str(path.relative_to(ROOT)) for path in matches)
        raise AssertionError(
            f"expected exactly one {expected!r} in expanded overview; "
            f"found {len(matches)}" + (f" at {locations}" if locations else "")
        )


def main() -> int:
    expansion = SkinExpansion()
    expansion.expand(ENTRY)

    expected_selectors = [
        Selector(rack=1, unit=unit, slot=slot, source=Path("."))
        for unit in (1, 2)
        for slot in (1, 2, 3)
    ]
    actual_identity = sorted(
        (selector.rack, selector.unit, selector.slot)
        for selector in expansion.selectors
    )
    expected_identity = sorted(
        (selector.rack, selector.unit, selector.slot)
        for selector in expected_selectors
    )
    if actual_identity != expected_identity:
        details = ", ".join(
            f"rack={selector.rack}/unit={selector.unit}/slot={selector.slot} "
            f"from {selector.source.relative_to(ROOT)}"
            for selector in expansion.selectors
        ) or "none"
        raise AssertionError(
            "overview must expose exactly three native EffectSelectors for "
            "Unit 1 / Player A and three for Unit 2 / Player B; found " + details
        )

    # Keep every interactive/display control on the same authoritative rack,
    # unit and slot as its EffectSelector. These keys also prove both bars can
    # be used, not merely display a selector label.
    for unit in (1, 2):
        require_key(expansion, f"[EffectRack1_EffectUnit{unit}],mix")
        require_key(
            expansion,
            f"[EffectRack1_EffectUnit{unit}],group_[Channel{unit}]_enable",
        )
        for slot in (1, 2, 3):
            prefix = f"[EffectRack1_EffectUnit{unit}_Effect{slot}]"
            require_key(expansion, prefix + ",enabled")
            require_key(expansion, prefix + ",meta")

    forbidden_routes = {
        "[EffectRack1_EffectUnit1],group_[Channel2]_enable",
        "[EffectRack1_EffectUnit2],group_[Channel1]_enable",
    }
    present_forbidden = sorted(
        value for value, _source in expansion.config_keys if value in forbidden_routes
    )
    if present_forbidden:
        raise AssertionError(
            "per-deck FX bar has cross-deck routing ownership: "
            + ", ".join(present_forbidden)
        )

    malformed = sorted(
        (value, source)
        for value, source in expansion.config_keys
        if "EffectRack1_EffectUnit" in value
        and not re.match(r"^\[EffectRack1_EffectUnit[12](?:_Effect[123])?\],", value)
    )
    if malformed:
        rendered = ", ".join(
            f"{value!r} in {source.relative_to(ROOT)}" for value, source in malformed
        )
        raise AssertionError("malformed or out-of-scope FX ConfigKey: " + rendered)

    deck_root = ET.parse(SKIN / "deck.xml").getroot()
    time_widgets = [
        widget
        for widget in deck_root.iter("NumberPos")
        if widget.findtext("ObjectName") == "DeckTrackTime"
    ]
    if len(time_widgets) != 1:
        raise AssertionError("zed deck must contain one full-size time display")
    if int(time_widgets[0].findtext("NumberOfDigits", "0")) < 10:
        raise AssertionError("time display must fit elapsed and remaining modes")
    minimum_width = int(
        time_widgets[0].findtext("MinimumSize", "0,0").split(",", 1)[0]
    )
    if minimum_width < 150:
        raise AssertionError("time display is too narrow for elapsed or remaining time")

    deck_text = (SKIN / "deck.xml").read_text(encoding="utf-8")
    if "<ObjectName>BeatJumps</ObjectName>" in deck_text or "BeatJumpSizeDisplay" in deck_text:
        raise AssertionError("redundant always-visible BEAT JUMP size box must stay removed")

    topbar_text = (SKIN / "topbar.xml").read_text(encoding="utf-8")
    if '<SetVariable name="config_key">beatjump</SetVariable>' not in topbar_text:
        raise AssertionError("dedicated BEAT JUMP tab must remain available")
    for deck_number in (1, 2):
        beatjump_text = (SKIN / f"beatjump_deck{deck_number}.xml").read_text(
            encoding="utf-8"
        )
        for amount in (4, 8, 16, 32):
            for direction in ("backward", "forward"):
                control = f"beatjump_{amount}_{direction}"
                if control not in beatjump_text:
                    raise AssertionError(
                        f"dedicated BEAT JUMP tab lost {control} for deck {deck_number}"
                    )

    print(
        "ZED_FX_LAYOUT_TEST_OK selectors=6 "
        "ownership=Unit1/Channel1,Unit2/Channel2 time=elapsed/remaining "
        "beatjump=hud-removed/tab-retained"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"ZED_FX_LAYOUT_TEST_FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
