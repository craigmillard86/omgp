"""Spec 002 T045 (#63) — the mechanical §9 timing-symbol → test map (SC-001).

`docs/trunk-link-layer.md` §9 is the conformance timing table; spec.md FR-029 says every row
of it is named by at least one boundary test, and SC-001 asks for that mapping *mechanically*
rather than by review. `specs/002-trunk-link-layer/contracts/tooling.md`
("tools/refimpl/test_timing_map.py", research R-12) fixes the mechanism: each boundary test
carries a Catch2 tag naming its symbol (`[timing:T_resp]`, …), this file reads the symbol
names from `protocol/omgp-protocol.yaml` (rule 1 — values are never restated here), and fails
naming any symbol whose tag appears in no `TEST_CASE` under `tests/unit/test_link_*.cpp` or
`tests/property/test_link_*.cpp`.

What this file establishes, and what it does not (CLAUDE.md rule 11):
- *Demonstrated* (`test_renaming_the_sole_site_of_a_symbol_fails_the_map`,
  `test_renaming_every_T_gap_site_fails_the_map`): a symbol that loses its tags is named by
  the failure — the map is not a no-op.
- *Demonstrated* (`test_renaming_one_of_many_T_gap_sites_leaves_the_map_green`): the gate is
  coverage-*existence* per symbol, not per test. With 28 `[timing:T_gap]` sites, renaming one
  leaves 27 and the map stays green; contracts/tooling.md's "rename one `[timing:T_gap]` tag"
  was written when one site per symbol was expected. The demonstration therefore renames
  every site of `T_gap`, plus the single site of a symbol that genuinely has only one.
- *Assumed*: that a present tag means the test actually exercises the symbol's boundary. A
  tag is a name, not a proof; what stops a value change passing silently is the tagged test
  itself reading the generated constant (quickstart.md §4: set `T_resp_us` to 199 and
  `./pipeline.sh codegen build unit` reds the `[timing:T_resp]` test). This file only stops
  such a test being dropped or renamed away unnoticed.

Run the listing on its own: `python3 -m pytest -q tools/refimpl/test_timing_map.py -s`.
"""
from __future__ import annotations

import pathlib
import re
import shutil

import pytest
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
YAML_PATH = ROOT / "protocol" / "omgp-protocol.yaml"

# The file set the tags must live in (contracts/tooling.md). Globs, not a list of names, so a
# new link test file joins the searched set by existing.
TEST_GLOBS = ("tests/unit/test_link_*.cpp", "tests/property/test_link_*.cpp")

# Symbol → Catch2 tag, exactly as contracts/tooling.md tabulates it. Keys are YAML key names
# (checked against the YAML by test_every_link_trunk_key_is_classified); no value is restated.
LINK_TRUNK_TAGS = {
    "bit_rate": "timing:bit_rate",
    "bit_rate_fallback": "timing:bit_rate_fallback",
    "T_turn_min_us": "timing:T_turn_min",
    "T_turn_max_us": "timing:T_turn_max",
    "T_resp_us": "timing:T_resp",
    "T_gap_us": "timing:T_gap",
    "T_poll_us": "timing:T_poll",
    "retries": "timing:retries",
}

# The ninth symbol lives under `limits`, not `link_trunk`: §9's "Max payload | 64 B | one L3
# message" row is `limits.max_l3_message` — the frame-payload budget — not the post-header
# `limits.max_l3_payload` (59) that the F10 split (#110/#154, 2026-09-06) carved out of it.
# contracts/tooling.md and tasks.md T045 both name max_l3_message; issue #63's acceptance
# criteria say max_l3_payload. The documents win (CLAUDE.md), and the tagged test agrees:
# tests/unit/test_link_types.cpp's `[timing:max_payload]` case is "kMaxWire is 142, derived
# from LIMIT_max_l3_message". The drift is flagged in the PR, not silently resolved.
LIMIT_TAGS = {"max_l3_message": "timing:max_payload"}

# Every other `link_trunk` key, with why it is not a §9 row. Listed so that ADDING a symbol to
# the YAML fails this file until someone decides whether it needs a boundary test — that is
# what "reads the link_trunk keys from the YAML" buys over a hand-kept list of nine.
NON_SECTION_9_KEYS = {
    "flag_byte": "§4 framing constant, not a §9 timing row (covered by the frame vectors)",
    "escape_byte": "§4 framing constant, not a §9 timing row",
    "escape_xor": "§4 framing constant, not a §9 timing row",
    "crc": "§4 names the CRC algorithm; not a §9 timing row",
    "suspect_after_failures": "§7 node health threshold; §9 tabulates neither health threshold",
    "offline_after_suspect_ms": "§7 node health threshold (tests tag it [timing:offline_after_suspect] "
                                "voluntarily; §9 does not list it, so this file does not require it)",
}

TAG_BY_SYMBOL = {**LINK_TRUNK_TAGS, **LIMIT_TAGS}

# The start of a Catch2 test declaration. Its arguments are then walked by hand rather than
# matched by a regex: the names in this tree run over several lines and contain commas,
# semicolons, parentheses and braces ("after a terminal Failed{Timeout}, …"), so every
# delimiter-excluding shortcut silently drops real declarations.
_TEST_CASE_RE = re.compile(r"\b(?:TEST_CASE|TEST_CASE_METHOD|TEMPLATE_TEST_CASE|SCENARIO)\s*\(")
_TAG_RE = re.compile(r"\[([^\[\]]+)\]")
# Catch2's tag argument is exactly a run of `[...]` groups and nothing else — the test that
# recognises it, so that a name happening to contain brackets is never read as tags.
_TAG_LITERAL_RE = re.compile(r"^\s*(?:\[[^\[\]]+\]\s*)+$")
_ANY_TIMING_TAG_RE = re.compile(r"\[timing:[^\[\]]*\]")


def _string_literals(text: str, open_paren: int):
    """The string literals of the argument list opening at `text[open_paren] == '('`.

    Walks to the matching `)`, skipping comments and character literals so that a `"`, `(`
    or `)` inside them cannot end the list early.
    """
    literals, depth, i, n = [], 1, open_paren + 1, len(text)
    while i < n and depth:
        c = text[i]
        if c == '"':
            buf, i = [], i + 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    buf.append(text[i:i + 2])
                    i += 2
                    continue
                buf.append(text[i])
                i += 1
            literals.append("".join(buf))
            i += 1
            continue
        if c == "'":                                  # char literal: '(', '"', '\\' …
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if text.startswith("//", i):
            nl = text.find("\n", i)
            i = n if nl < 0 else nl + 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i)
            i = n if end < 0 else end + 2
            continue
        depth += (c == "(") - (c == ")")
        i += 1
    return literals


def yaml_doc():
    return yaml.safe_load(YAML_PATH.read_text())


def searched_files(root: pathlib.Path):
    """Every file the tags may live in, sorted, relative paths resolved against `root`."""
    return sorted(p for g in TEST_GLOBS for p in root.glob(g))


def parse_test_cases(path: pathlib.Path):
    """[(test name, [tag, ...]), ...] for one Catch2 source file.

    Catch2 concatenates adjacent literals, so a name split over lines arrives as several
    literals; the tag literal is the one made only of `[...]` groups.
    """
    out, text = [], path.read_text()
    for m in _TEST_CASE_RE.finditer(text):
        literals = _string_literals(text, m.end() - 1)
        if not literals:
            continue
        name = "".join(lit for lit in literals if not _TAG_LITERAL_RE.match(lit)).strip()
        tags = [t for lit in literals if _TAG_LITERAL_RE.match(lit) for t in _TAG_RE.findall(lit)]
        out.append((name or literals[0], tags))
    return out


def timing_map(root: pathlib.Path = ROOT):
    """symbol -> [(file relative to root, TEST_CASE name), ...] for all nine §9 symbols."""
    found = {symbol: [] for symbol in TAG_BY_SYMBOL}
    by_tag = {tag: symbol for symbol, tag in TAG_BY_SYMBOL.items()}
    for path in searched_files(root):
        rel = path.relative_to(root).as_posix()
        for name, tags in parse_test_cases(path):
            for tag in tags:
                if tag in by_tag:
                    found[by_tag[tag]].append((rel, name))
    return found


def missing_symbols(mapping):
    return sorted(symbol for symbol, hits in mapping.items() if not hits)


def format_map(mapping) -> str:
    """SC-001's listing: every symbol, its tag, and the tests that carry it."""
    lines = []
    for symbol in sorted(TAG_BY_SYMBOL, key=str.lower):
        hits = mapping[symbol]
        lines.append(f"{symbol} -> [{TAG_BY_SYMBOL[symbol]}]  ({len(hits)} test case(s))")
        for rel, name in hits:
            lines.append(f'    {rel}: "{name}"')
        if not hits:
            lines.append("    *** NO TEST CARRIES THIS TAG ***")
    return "\n".join(lines)


def mirror_test_tree(root: pathlib.Path, dest: pathlib.Path) -> pathlib.Path:
    """Copy just the searched file set into `dest`, keeping relative paths, and return it."""
    for src in searched_files(root):
        out = dest / src.relative_to(root)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, out)
    return dest


# --- the map itself -------------------------------------------------------------------------


def test_every_link_trunk_key_is_classified():
    """Symbol names come from the YAML, so a new/renamed one cannot slip past unmapped."""
    doc = yaml_doc()
    link_trunk = doc["link_trunk"]
    assert set(LINK_TRUNK_TAGS) | set(NON_SECTION_9_KEYS) == set(link_trunk), (
        "protocol/omgp-protocol.yaml link_trunk keys and this file's classification have "
        f"diverged: unclassified={sorted(set(link_trunk) - set(LINK_TRUNK_TAGS) - set(NON_SECTION_9_KEYS))}, "
        f"gone from the YAML={sorted((set(LINK_TRUNK_TAGS) | set(NON_SECTION_9_KEYS)) - set(link_trunk))}"
    )
    assert set(LIMIT_TAGS) <= set(doc["limits"])
    assert len(TAG_BY_SYMBOL) == 9      # contracts/tooling.md: eight link_trunk symbols + the payload cap


def test_the_searched_file_set_is_not_empty():
    """Blind spot guard: with no matching file every tag would be 'missing', not 'green' —
    but a typo'd glob plus a lenient assertion is how such a gate silently dies."""
    files = searched_files(ROOT)
    assert files, f"no file matched {TEST_GLOBS} under {ROOT}"
    assert any("property/" in p.as_posix() for p in files)
    assert any("unit/" in p.as_posix() for p in files)


def test_every_timing_symbol_has_a_boundary_test():
    """FR-029/SC-001: every §9 row is named by at least one tagged test."""
    mapping = timing_map(ROOT)
    assert not missing_symbols(mapping), (
        "trunk §9 rows with no boundary test carrying their tag: "
        + ", ".join(f"{s} ([{TAG_BY_SYMBOL[s]}])" for s in missing_symbols(mapping))
        + "\n" + format_map(mapping)
    )


def test_prints_the_symbol_to_test_case_map():
    """SC-001's 'mechanical listing' — visible with `pytest -s` (quickstart.md §4)."""
    mapping = timing_map(ROOT)
    print("\ntrunk §9 timing symbol -> tagged Catch2 test cases\n")
    print(format_map(mapping))
    assert sum(len(h) for h in mapping.values()) >= len(TAG_BY_SYMBOL)


def test_every_timing_tag_in_the_file_set_is_attributed_to_a_test_case():
    """Parser control: if a `[timing:…]` tag existed in a form `parse_test_cases` cannot read,
    the map would under-count silently — and a symbol's last site could be lost without the
    map noticing. Every occurrence in the searched files must be one attributed to a
    TEST_CASE, with multiplicity. (It caught two real parser misses while this file was
    written: a name containing `;` and a name containing `Failed{Timeout}`.)"""
    for path in searched_files(ROOT):
        in_text = _ANY_TIMING_TAG_RE.findall(path.read_text())
        parsed = [f"[{t}]" for _, tags in parse_test_cases(path) for t in tags
                  if t.startswith("timing:")]
        assert sorted(in_text) == sorted(parsed), (
            f"{path.relative_to(ROOT)}: timing tags in the text that no TEST_CASE parse "
            f"accounts for: {sorted(set(in_text) - set(parsed))} (and vice versa: "
            f"{sorted(set(parsed) - set(in_text))})"
        )


# --- the discriminating checks: prove the map bites ------------------------------------------


def _rename_tag(path: pathlib.Path, tag: str, count: int) -> int:
    text = path.read_text()
    new, n = re.subn(re.escape(f"[{tag}]"), "[timing:T_gpa_MISSPELLED]", text, count=count)
    path.write_text(new)
    return n


def _sites(mapping, symbol):
    return mapping[symbol]


def test_renaming_the_sole_site_of_a_symbol_fails_the_map(tmp_path):
    """contracts/tooling.md's discriminator, on a symbol that genuinely has one site: rename
    that one tag and the map fails NAMING the symbol. Were the check a no-op, the assertion
    below would report `missing == []` and this test would fail."""
    real = timing_map(ROOT)
    singles = [s for s in TAG_BY_SYMBOL if len(real[s]) == 1]
    if not singles:
        pytest.skip("no §9 symbol currently has exactly one tagged test")
    symbol = singles[0]
    rel = real[symbol][0][0]

    root = mirror_test_tree(ROOT, tmp_path)
    assert _rename_tag(root / rel, TAG_BY_SYMBOL[symbol], count=1) == 1
    missing = missing_symbols(timing_map(root))
    # Against the real tree's own missing set, not against []: this case must report what the
    # RENAME cost, whether or not the tree is otherwise complete today.
    expected = sorted(set(missing_symbols(real)) | {symbol})
    assert missing == expected, f"expected the map to name {symbol}; it named {missing}"


def test_renaming_every_T_gap_site_fails_the_map(tmp_path):
    """The contract names `[timing:T_gap]` specifically. It has many sites today, so the
    demonstration renames all of them: the map then fails naming `T_gap_us` and nothing else."""
    real = timing_map(ROOT)
    sites = _sites(real, "T_gap_us")
    assert sites, "T_gap_us has no tagged test — the discriminator cannot be demonstrated"

    root = mirror_test_tree(ROOT, tmp_path)
    renamed = sum(_rename_tag(root / rel, "timing:T_gap", count=0)
                  for rel in sorted({rel for rel, _ in sites}))
    assert renamed >= len(sites)
    missing = missing_symbols(timing_map(root))
    expected = sorted(set(missing_symbols(real)) | {"T_gap_us"})
    assert missing == expected, f"expected the map to name T_gap_us; it named {missing}"


def test_renaming_one_of_many_T_gap_sites_leaves_the_map_green(tmp_path):
    """Honest statement of the gate's reach (CLAUDE.md rule 11): it asserts per-symbol
    coverage, so with 28 `[timing:T_gap]` sites, losing one is invisible here. If this test
    ever fails because `T_gap_us` dropped to a single site, the sole-site case above is the
    one that then carries the contract's exact wording."""
    real = timing_map(ROOT)
    sites = _sites(real, "T_gap_us")
    if len(sites) < 2:
        pytest.skip("T_gap_us has fewer than two tagged tests; the sole-site case covers it")

    root = mirror_test_tree(ROOT, tmp_path)
    assert _rename_tag(root / sites[0][0], "timing:T_gap", count=1) == 1
    mapping = timing_map(root)
    assert missing_symbols(mapping) == missing_symbols(real)   # unchanged: nothing new is named
    assert len(mapping["T_gap_us"]) == len(sites) - 1
