#!/usr/bin/env python3
"""Fills translations of a .po from a JSON file.

Usage: tools/fill_po.py po/pt_BR.po translations.json

The JSON maps each msgid to its translation: a string, or a list of
strings for plural entries (msgstr[0], msgstr[1]...). Filled are entries
that are empty and entries msgmerge marked fuzzy (the English changed);
their fuzzy flag goes away. Translated entries are kept. Prints the
msgids still untranslated, and the JSON keys that matched nothing."""
import json
import re
import sys


def unquote(parts):
    return "".join(json.loads(p) for p in parts)


def quote(s):
    return json.dumps(s, ensure_ascii=False)


def parse(text):
    """Entries as lists of lines; blank lines separate them."""
    return [block.split("\n") for block in re.split(r"\n\n+", text.strip("\n"))]


def field(lines, name):
    """The quoted parts of `name "..."` and its continuation lines."""
    for i, line in enumerate(lines):
        if line.startswith(name + " "):
            parts = [line[len(name) + 1:]]
            for cont in lines[i + 1:]:
                if not cont.startswith('"'):
                    break
                parts.append(cont)
            return parts
    return None


def main():
    path, table_path = sys.argv[1], sys.argv[2]
    table = json.load(open(table_path, encoding="utf-8"))
    entries = parse(open(path, encoding="utf-8").read())
    used, missing = set(), []
    out = []
    for lines in entries:
        msgid_parts = field(lines, "msgid")
        if msgid_parts is None or lines[0].startswith("#~"):
            out.append(lines)
            continue
        msgid = unquote(msgid_parts)
        if not msgid:  # the header
            out.append(lines)
            continue
        plural = field(lines, "msgid_plural") is not None
        fuzzy = any(l.startswith("#,") and "fuzzy" in l for l in lines)
        if plural:
            current = [unquote(field(lines, "msgstr[%d]" % n) or ['""']) for n in range(2)]
            empty = not all(current)
        else:
            empty = not unquote(field(lines, "msgstr") or ['""'])
        if (empty or fuzzy) and msgid in table:
            used.add(msgid)
            keep = []
            for l in lines:
                if l.startswith("msgstr"):
                    break  # msgstr and its continuations end the entry
                if l.startswith("#|"):  # previous msgid, from msgmerge
                    continue
                if l.startswith("#,"):
                    flags = [f.strip() for f in l[2:].split(",")]
                    flags = [f for f in flags if f and f != "fuzzy"]
                    if not flags:
                        continue
                    l = "#, " + ", ".join(flags)
                keep.append(l)
            value = table[msgid]
            if plural:
                values = value if isinstance(value, list) else [value, value]
                for n, v in enumerate(values):
                    keep.append("msgstr[%d] %s" % (n, quote(v)))
            else:
                keep.append("msgstr " + quote(value))
            out.append(keep)
            continue
        if empty or fuzzy:
            missing.append(msgid)
        out.append(lines)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n\n".join("\n".join(l) for l in out) + "\n")
    for m in missing:
        print("untranslated:", m[:70].replace("\n", "\\n"), file=sys.stderr)
    for k in table:
        if k not in used:
            print("not used:", k[:70].replace("\n", "\\n"), file=sys.stderr)


main()
