#!/usr/bin/env python3
# gen_charset_tables.py - generate src/charset_tables_data.c (+ the bijection
# matrix doc) from the vendored Unicode Consortium MICSFT mappings.
#
# DEV-TIME ONLY. Not built or shipped. The committed output (the C data file)
# is what the device uses; this generator exists so the tables are reproducible
# and auditable against their primary source (vendor/charset-mappings/, the
# strict MICSFT mappings - NOT WindowsBestFit). Re-run after a data refresh:
#
#     python3 tools/gen_charset_tables.py
#
# The hand-written decode/encode LOGIC lives in src/charset_tables.c; this
# emits only DATA (the forward tables, the sorted reverse maps, the registry).
#
# This is free and unencumbered software released into the public domain.

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "vendor", "charset-mappings")
OUT_C = os.path.join(ROOT, "src", "charset_tables_data.c")
OUT_DOC = os.path.join(ROOT, "docs", "charset-bijection.md")

REPLACEMENT = 0xFFFD

# The baked pages. (codepage, subdir) - the subdir is the MICSFT layout
# (WINDOWS/ for the ANSI + DBCS pages, PC/ for the OEM/DOS pages). cp858 is
# special-cased (derived from cp850; absent upstream).
ANSI = [1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 874]
DBCS = [932, 936, 949, 950]
OEM = [437, 737, 775, 850, 852, 855, 857, 860, 861, 862, 863, 864, 865, 866, 869]
# cp858 = cp850 + Euro at 0xD5 (derived; see vendor README + the bijection doc).
DERIVED = {858: (850, 0xD5, 0x20AC)}


def parse_file(path):
    """Return list of (mb_int, cu_int_or_None, is_lead) from a MICSFT file.

    mb_int is the first column (a byte 0x00-0xFF or a double-byte 0xLLTT).
    cu is the mapped code unit or None (undefined / a bare DBCS lead marker).
    is_lead is True for a '#DBCS LEAD BYTE' row.
    """
    rows = []
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            s = line.rstrip("\n")
            if not s or s.lstrip().startswith("#"):
                continue
            parts = s.split("\t")
            # Column 0 is the byte(s); column 1 is the codepoint or empty.
            c0 = parts[0].strip()
            if not c0.lower().startswith("0x"):
                continue
            mb = int(c0, 16)
            cu = None
            if len(parts) > 1 and parts[1].strip().lower().startswith("0x"):
                cu = int(parts[1].strip(), 16)
            is_lead = "DBCS LEAD BYTE" in s
            rows.append((mb, cu, is_lead))
    return rows


class Page:
    def __init__(self, cp, is_dbcs):
        self.cp = cp
        self.is_dbcs = is_dbcs
        self.dec1 = [REPLACEMENT] * 256        # single-byte / non-lead units
        self.lead = [0] * 256                  # 1 == lead byte (DBCS)
        self.dec2 = {}                         # lead -> [256] trail units
        self.undefined_sbcs = []               # documented undefined positions
        # forward (mb_int -> cu) used to build the reverse map + bijection report
        self.fwd = {}

    def set_single(self, b, cu):
        self.dec1[b] = cu if cu is not None else REPLACEMENT
        if cu is None:
            self.undefined_sbcs.append(b)
        else:
            self.fwd[b] = cu

    def set_lead(self, b):
        self.lead[b] = 1
        if b not in self.dec2:
            self.dec2[b] = [REPLACEMENT] * 256

    def set_double(self, mb, cu):
        lead = (mb >> 8) & 0xFF
        trail = mb & 0xFF
        self.lead[lead] = 1
        if lead not in self.dec2:
            self.dec2[lead] = [REPLACEMENT] * 256
        self.dec2[lead][trail] = cu if cu is not None else REPLACEMENT
        if cu is not None:
            self.fwd[mb] = cu


def build_page(cp, subdir, is_dbcs):
    path = os.path.join(DATA, subdir, "CP%d.TXT" % cp)
    rows = parse_file(path)
    p = Page(cp, is_dbcs)
    for mb, cu, is_lead in rows:
        if mb <= 0xFF:
            if is_lead:
                p.set_lead(mb)
            else:
                p.set_single(mb, cu)
        else:
            assert mb >= 0x8100, "cp%d: double-byte lead < 0x81 breaks the " \
                "mb>0xFF <=> 2-byte invariant (mb=0x%04X)" % (cp, mb)
            p.set_double(mb, cu)
    return p


def build_derived(cp):
    base_cp, byte, new_cu = DERIVED[cp]
    p = build_page(base_cp, "PC", False)
    p.cp = cp
    # Apply the single documented override (e.g. cp858 = cp850 + Euro @ 0xD5).
    old = p.dec1[byte]
    p.dec1[byte] = new_cu
    if old in (REPLACEMENT,):
        if byte in p.undefined_sbcs:
            p.undefined_sbcs.remove(byte)
    p.fwd.pop(byte, None)
    # remove the stale reverse contribution of the old cu at this byte by
    # rebuilding fwd from dec1 (SBCS only)
    p.fwd = {b: u for b, u in enumerate(p.dec1) if u != REPLACEMENT}
    p.fwd[byte] = new_cu
    p._derived = (base_cp, byte, old, new_cu)
    return p


def build_reverse(p):
    """canonical reverse: cu -> lowest mb that decodes to it. Returns
    (rev_sorted, collisions) where collisions maps cu -> sorted list of all
    mbs (len>1 are the non-bijective duplicate rows)."""
    by_cu = {}
    for mb, cu in p.fwd.items():
        by_cu.setdefault(cu, []).append(mb)
    rev = {}
    collisions = {}
    for cu, mbs in by_cu.items():
        mbs.sort()
        rev[cu] = mbs[0]            # canonical = lowest byte sequence
        if len(mbs) > 1:
            collisions[cu] = mbs
    rev_sorted = sorted(rev.items())  # by cu
    return rev_sorted, collisions


# ---- C emission ----------------------------------------------------------

def c_u16_array(name, values):
    out = ["static const unsigned short %s[%d] = {" % (name, len(values))]
    line = "    "
    for v in values:
        tok = "0x%04X," % v
        if len(line) + len(tok) > 78:
            out.append(line)
            line = "    "
        line += tok
    if line.strip():
        out.append(line)
    out.append("};")
    return "\n".join(out)


def emit():
    pages = []
    for cp in ANSI:
        pages.append(build_page(cp, "WINDOWS", False))
    for cp in DBCS:
        pages.append(build_page(cp, "WINDOWS", True))
    for cp in OEM:
        pages.append(build_page(cp, "PC", False))
    for cp in sorted(DERIVED):
        pages.append(build_derived(cp))

    reverse = {}
    collisions = {}
    for p in pages:
        rev_sorted, coll = build_reverse(p)
        reverse[p.cp] = rev_sorted
        collisions[p.cp] = coll

    out = []
    out.append("/*")
    out.append(" * charset_tables_data.c - GENERATED by tools/gen_charset_tables.py")
    out.append(" * from the vendored Unicode Consortium MICSFT mappings")
    out.append(" * (vendor/charset-mappings/). DO NOT EDIT - regenerate instead.")
    out.append(" *")
    out.append(" * %d baked code pages: %d ANSI + %d OEM/DOS single-byte + %d CJK"
               % (len(pages), len(ANSI), len(OEM) + len(DERIVED), len(DBCS)))
    out.append(" * DBCS. The decode/encode logic is hand-written in"
               " src/charset_tables.c;")
    out.append(" * this file is cited DATA, not logic.")
    out.append(" *")
    out.append(" * Public domain (Unlicense); the mapping data is from the"
               " Unicode")
    out.append(" * Consortium (see vendor/charset-mappings/README.md).")
    out.append(" */")
    out.append('#include "charset_internal.h"')
    out.append('#include "charset_tables.h"')
    out.append("")

    for p in pages:
        tag = "cp%d" % p.cp
        out.append("/* ---- code page %d ---- */" % p.cp)
        out.append(c_u16_array("dec1_%s" % tag, p.dec1))
        if p.is_dbcs:
            leads = sorted(p.dec2.keys())
            leadrow = [-1] * 256
            flat = []
            for row_idx, lead in enumerate(leads):
                leadrow[lead] = row_idx
                flat.extend(p.dec2[lead])
            out.append("static const unsigned char lead_%s[256] = {" % tag)
            line = "    "
            for v in p.lead:
                tok = "%d," % v
                if len(line) + len(tok) > 78:
                    out.append(line); line = "    "
                line += tok
            out.append(line); out.append("};")
            out.append("static const short leadrow_%s[256] = {" % tag)
            line = "    "
            for v in leadrow:
                tok = "%d," % v
                if len(line) + len(tok) > 78:
                    out.append(line); line = "    "
                line += tok
            out.append(line); out.append("};")
            out.append(c_u16_array("dec2_%s" % tag, flat))
            ndec2 = len(leads)
        else:
            ndec2 = 0
        # reverse map
        rev = reverse[p.cp]
        out.append("static const CpRevEntry rev_%s[%d] = {" % (tag, len(rev)))
        line = "    "
        for cu, mb in rev:
            tok = "{0x%04X,0x%04X}," % (cu, mb)
            if len(line) + len(tok) > 78:
                out.append(line); line = "    "
            line += tok
        if line.strip():
            out.append(line)
        out.append("};")
        out.append("")

    # registry
    out.append("const CpPage charset_pages[] = {")
    for p in pages:
        tag = "cp%d" % p.cp
        if p.is_dbcs:
            leads = sorted(p.dec2.keys())
            out.append("    {%d,1,dec1_%s,lead_%s,leadrow_%s,dec2_%s,%d,"
                       "rev_%s,%d}," % (p.cp, tag, tag, tag, tag, len(leads),
                                        tag, len(reverse[p.cp])))
        else:
            out.append("    {%d,0,dec1_%s,0,0,0,0,rev_%s,%d},"
                       % (p.cp, tag, tag, len(reverse[p.cp])))
    out.append("};")
    out.append("const int charset_page_count ="
               " (int)(sizeof(charset_pages)/sizeof(charset_pages[0]));")
    out.append("")

    with open(OUT_C, "w") as f:
        f.write("\n".join(out))

    emit_doc(pages, reverse, collisions)
    # summary to stderr
    total_rev = sum(len(reverse[p.cp]) for p in pages)
    sys.stderr.write("generated %s: %d pages, %d reverse entries total\n"
                     % (os.path.relpath(OUT_C, ROOT), len(pages), total_rev))


def emit_doc(pages, reverse, collisions):
    d = []
    d.append("# Code-page non-bijection matrix (generated)\n")
    d.append("Generated by `tools/gen_charset_tables.py` from the vendored "
             "Unicode\nConsortium MICSFT mappings. For each baked code page, "
             "this records where\n`codepage -> Unicode -> codepage` is **not** "
             "the identity, i.e. the carved-out\nexceptions to "
             "`CodepageRoundTripsOnBijectiveSubset` "
             "(`specs/encoding.allium`).\n")
    d.append("Two kinds of non-bijection:\n")
    d.append("- **Undefined byte positions** decode to U+FFFD "
             "(`codepage_to_unicode` is\n  total) and have no reverse, so they "
             "do not round-trip.\n")
    d.append("- **Duplicate rows**: several byte sequences decode to the same "
             "code unit\n  (vendor-divergent / NEC-IBM / compatibility rows). "
             "Only the **canonical**\n  sequence (the lowest byte value) "
             "re-encodes; the others are the documented\n  non-round-tripping "
             "exceptions. The device applies the canonical mapping, so\n  an "
             "inbound path round-trips to the bytes the OS itself uses.\n")
    d.append("The encode direction is **strict**: a code unit with no byte "
             "representation\nis rejected (never `'?'`-substituted) - "
             "`StrictNarrowingRejectsUnrepresentable`.\n")
    d.append("## cp858 (derived)\n")
    d.append("cp858 is **not** in the upstream MICSFT/PC set. It is derived "
             "from cp850 by\nthe single documented override `0xD5` -> `U+20AC` "
             "(EURO SIGN), which in cp850\nis `U+0131` (LATIN SMALL LETTER "
             "DOTLESS I). Every other position is cp850.\n")
    d.append("## Per-page summary\n")
    d.append("| CP | kind | undefined bytes | code units with >1 byte form |")
    d.append("|---:|------|----------------:|-----------------------------:|")
    for p in pages:
        kind = "DBCS" if p.is_dbcs else "SBCS"
        nund = len([b for b in range(256) if p.dec1[b] == REPLACEMENT and
                    (not p.is_dbcs or not p.lead[b])])
        d.append("| %d | %s | %d | %d |"
                 % (p.cp, kind, nund, len(collisions[p.cp])))
    d.append("")
    d.append("## Undefined single-byte positions (SBCS + DBCS single-byte "
             "range)\n")
    for p in pages:
        und = [b for b in range(256) if p.dec1[b] == REPLACEMENT and
               (not p.is_dbcs or not p.lead[b])]
        if und:
            d.append("- **cp%d**: %s"
                     % (p.cp, " ".join("0x%02X" % b for b in und)))
    d.append("")
    d.append("## Notable named cases\n")
    d.append("- **cp932 wave-dash / fullwidth tilde**: `0x8160` decodes to "
             "**U+FF5E**\n  (FULLWIDTH TILDE), the Microsoft-canonical "
             "mapping, *not* JIS U+301C. This\n  is the documented "
             "vendor-divergent choice; the device applies it as-is.\n")
    # a couple of concrete cp932 collision examples for the doc
    for p in pages:
        if p.cp != 932:
            continue
        coll = collisions[p.cp]
        sample = sorted(coll.items())[:8]
        if sample:
            d.append("- **cp932 duplicate rows** (canonical = first; %d code "
                     "units affected). Examples:" % len(coll))
            for cu, mbs in sample:
                d.append("  - U+%04X <- %s (canonical 0x%04X)"
                         % (cu, " ".join("0x%04X" % m for m in mbs), mbs[0]))
    d.append("")
    with open(OUT_DOC, "w") as f:
        f.write("\n".join(d))


if __name__ == "__main__":
    emit()
