# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict proto2/proto3 schema parser used by the protobuf audit (Issue #62).

Dependency-free. Parses the subset of the protobuf language that the pinned
AASDK and Open Android Auto trees use and *fails loudly* on anything else:
every unrecognised character or construct raises ``ProtoParseError`` carrying
``<file>:<line>``. Nothing is silently skipped.

Supported: syntax, package, import [public|weak], file/message/enum/field
options (including parenthesised custom option names), message (nested),
fields (required/optional/repeated, proto3 implicit, ``map<K,V>``), oneof,
enum (+values, value options, ``allow_alias``), ``reserved`` (numbers, ranges,
names), ``extensions`` ranges, ``service``/``rpc`` (recorded, no wire meaning).
Unsupported and therefore an error: ``extend``, ``group``, aggregate option
values, ``stream`` anywhere outside ``rpc``.
"""
from __future__ import annotations

import re
from typing import Any, Final

MAX_FIELD_NUMBER: Final[int] = 536870911

SCALAR_WIRE: Final[dict[str, str]] = {
    "int32": "varint", "int64": "varint", "uint32": "varint", "uint64": "varint",
    "sint32": "varint", "sint64": "varint", "bool": "varint",
    "fixed64": "i64", "sfixed64": "i64", "double": "i64",
    "fixed32": "i32", "sfixed32": "i32", "float": "i32",
    "string": "len", "bytes": "len",
}
PACKABLE: Final[frozenset[str]] = frozenset(
    t for t, w in SCALAR_WIRE.items() if w != "len"
)

_TOKEN_RE: Final = re.compile(
    r"(?P<ws>\s+)"
    r"|(?P<lc>//[^\n]*)"
    r"|(?P<bc>/\*.*?\*/)"
    r"|(?P<str>\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*')"
    r"|(?P<num>(?:0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+(?:[eE][-+]?\d+)?))"
    r"|(?P<id>\.?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)"
    r"|(?P<sym>[{}()\[\];=,<>:+\-])",
    re.DOTALL,
)


class ProtoParseError(ValueError):
    """Raised for any construct the parser cannot account for."""

    def __init__(self, path: str, line: int, message: str) -> None:
        super().__init__(f"{path}:{line}: {message}")
        self.path = path
        self.line = line
        self.detail = message


Token = tuple[str, str, int]  # (kind, text, line)

_CONFIDENCE_RE: Final = re.compile(r"\bconfidence\s*:\s*([a-zA-Z0-9_-]+)", re.I)
_RETRACTED_RE: Final = re.compile(r"\bretracted\b", re.I)
_MSG_ID_RE: Final = re.compile(
    r"(?:wire\s+msg(?:\s+id)?|msg(?:\s+id)?|message\s+id|ordinal)\s*[:=#]?\s*(0x[0-9a-fA-F]+|\d+)",
    re.I,
)
_HEX_MSG_RE: Final = re.compile(r"\b(0x8[0-9a-fA-F]{3})\b")
_DIR_RE: Final = re.compile(
    r"(Phone\s*(?:->|→)\s*HU|HU\s*(?:->|→)\s*Phone|Bidirectional)",
    re.I,
)
_VER_RE: Final = re.compile(
    r"(?:AA\s*v?(\d+\.\d+)|Android\s+Auto\s+(\d+\.\d+)|PDK\s*<?\s*(\d+\.\d+)|HAL\s+(\d+\.\d+)|\b(16\.[12]|17\.[0-9])\b)",
    re.I,
)
_DEPRECATED_CMT_RE: Final = re.compile(r"@deprecated|\bdeprecated\b", re.I)


def _strip_comment_lines(text: str, start_line: int) -> list[tuple[int, str]]:
    if text.startswith("//"):
        return [(start_line, text[2:].strip())]
    if text.startswith("/*") and text.endswith("*/"):
        lines: list[tuple[int, str]] = []
        raw_lines = text[2:-2].splitlines()
        if not raw_lines:
            return [(start_line, "")]
        for offset, raw in enumerate(raw_lines):
            s = raw.strip().lstrip("*").strip()
            lines.append((start_line + offset, s))
        return lines
    return []


def _normalize_dir(raw: str) -> str:
    low = raw.lower()
    if "bidirectional" in low:
        return "Bidirectional"
    if low.startswith("phone"):
        return "Phone->HU"
    if low.startswith("hu"):
        return "HU->Phone"
    return raw


def _format_annotation_field(sources_by_val: dict[str, set[tuple[str, str, str, int]]]) -> dict[str, Any]:
    if not sources_by_val:
        return {"status": "unknown", "values": []}
    return {
        "status": "known",
        "values": [
            {
                "value": val,
                "sources": [
                    {"kind": kind, "scope": scope, "file": fpath, "line": ln}
                    for kind, scope, fpath, ln in sorted(srcs, key=lambda x: (x[0], x[2], x[3], x[1]))
                ],
            }
            for val, srcs in sorted(sources_by_val.items())
        ],
    }


def annotation_values(ann: dict[str, Any] | None, field: str) -> list[str]:
    """Extract sorted list of string values from either structured or legacy annotation field."""
    if not ann:
        return []
    val = ann.get(field)
    if isinstance(val, dict):
        return [str(item["value"]) for item in val.get("values", []) if isinstance(item, dict) and "value" in item]
    if isinstance(val, list):
        return [str(x) for x in val]
    return []


def _parse_comment_entries(
    entries: list[tuple[int, str, str]], file_path: str,
) -> dict[str, Any]:
    conf_srcs: dict[str, set[tuple[str, str, str, int]]] = {}
    id_srcs: dict[str, set[tuple[str, str, str, int]]] = {}
    dir_srcs: dict[str, set[tuple[str, str, str, int]]] = {}
    ver_srcs: dict[str, set[tuple[str, str, str, int]]] = {}
    retracted = False
    retraction_notes: list[str] = []
    retraction_srcs: set[tuple[str, str, str, int]] = set()
    deprecated = False

    for line_num, scope, line in entries:
        if not line:
            continue
        src = ("proto_comment", scope, file_path, line_num)
        for m in _CONFIDENCE_RE.finditer(line):
            val = m.group(1).lower()
            conf_srcs.setdefault(val, set()).add(src)
            if val == "retracted":
                retracted = True
                retraction_srcs.add(src)
        if _RETRACTED_RE.search(line):
            retracted = True
            retraction_srcs.add(src)
            if "retraction" not in line.lower() or "retained" not in line.lower():
                if len(retraction_notes) < 2 and line not in retraction_notes:
                    retraction_notes.append(line)
        for m in _MSG_ID_RE.finditer(line):
            raw_id = m.group(1)
            norm_id = f"0x{int(raw_id, 16):04X}" if raw_id.lower().startswith("0x") else raw_id
            id_srcs.setdefault(norm_id, set()).add(src)
        for m in _HEX_MSG_RE.finditer(line):
            norm_id = f"0x{int(m.group(1), 16):04X}"
            id_srcs.setdefault(norm_id, set()).add(src)
        for m in _DIR_RE.finditer(line):
            nd = _normalize_dir(m.group(1))
            dir_srcs.setdefault(nd, set()).add(src)
        for m in _VER_RE.finditer(line):
            v = next((g for g in m.groups() if g), "")
            if v:
                prefix = "PDK " if "pdk" in m.group(0).lower() else ("HAL " if "hal" in m.group(0).lower() else "AA ")
                tag = f"{prefix}{v}"
                ver_srcs.setdefault(tag, set()).add(src)
        if _DEPRECATED_CMT_RE.search(line):
            deprecated = True

    return {
        "confidence": _format_annotation_field(conf_srcs),
        "retracted": retracted,
        "retraction_note": "; ".join(retraction_notes),
        "retraction_sources": [
            {"kind": kind, "scope": scope, "file": fpath, "line": ln}
            for kind, scope, fpath, ln in sorted(retraction_srcs, key=lambda x: (x[0], x[2], x[3], x[1]))
        ],
        "wire_msg_ids": _format_annotation_field(id_srcs),
        "directions": _format_annotation_field(dir_srcs),
        "version_gates": _format_annotation_field(ver_srcs),
        "deprecated_comment": deprecated,
    }


def extract_file_comments(content: str, path: str) -> dict[str, Any]:
    """Extract per-line comment lines and file-level structured annotations."""
    line_comments: dict[int, list[str]] = {}
    comment_lines: dict[int, list[str]] = {}
    block_start_by_end: dict[int, int] = {}
    code_lines: set[int] = set()
    all_entries: list[tuple[int, str, str]] = []
    pos = 0
    line = 1
    n = len(content)
    while pos < n:
        m = _TOKEN_RE.match(content, pos)
        if m is None:
            break
        kind = m.lastgroup or ""
        text = m.group(0)
        span_lines = text.count("\n")
        if kind in ("lc", "bc"):
            stripped_with_lines = _strip_comment_lines(text, line)
            end_line = line + span_lines
            if kind == "bc":
                prev = block_start_by_end.get(end_line)
                block_start_by_end[end_line] = min(prev, line) if prev is not None else line
            for ln, s in stripped_with_lines:
                comment_lines.setdefault(ln, []).append(s)
                if s:
                    line_comments.setdefault(end_line, []).append(s)
                    all_entries.append((ln, "file_comment", s))
        elif kind != "ws":
            for ln in range(line, line + span_lines + 1):
                code_lines.add(ln)
        line += span_lines
        pos = m.end()
    return {
        "file_annotations": _parse_comment_entries(all_entries, path),
        "line_comments": line_comments,
        "comment_lines": comment_lines,
        "block_start_by_end": block_start_by_end,
        "code_lines": code_lines,
    }


def tokenize(content: str, path: str) -> list[Token]:
    """Contiguous scan: every character is either consumed or an error."""
    tokens: list[Token] = []
    pos = 0
    line = 1
    n = len(content)
    while pos < n:
        m = _TOKEN_RE.match(content, pos)
        if m is None:
            if content.startswith("/*", pos):
                raise ProtoParseError(path, line, "unterminated block comment")
            ch = content[pos]
            if ch in "\"'":
                raise ProtoParseError(path, line, "unterminated string literal")
            raise ProtoParseError(path, line, f"unrecognised input {content[pos:pos + 12]!r}")
        kind = m.lastgroup or ""
        text = m.group(0)
        if kind not in ("ws", "lc", "bc"):
            tokens.append((kind, text, line))
        line += text.count("\n")
        pos = m.end()
    return tokens


class _Parser:
    def __init__(self, tokens: list[Token], path: str) -> None:
        self.t = tokens
        self.i = 0
        self.path = path

    # -- token helpers ---------------------------------------------------
    def err(self, msg: str, line: int | None = None) -> ProtoParseError:
        if line is None:
            line = self.t[self.i][2] if self.i < len(self.t) else (self.t[-1][2] if self.t else 1)
        return ProtoParseError(self.path, line, msg)

    def peek(self, k: int = 0) -> str | None:
        j = self.i + k
        return self.t[j][1] if j < len(self.t) else None

    def line(self) -> int:
        return self.t[self.i][2] if self.i < len(self.t) else (self.t[-1][2] if self.t else 1)

    def kind(self) -> str | None:
        return self.t[self.i][0] if self.i < len(self.t) else None

    def next(self) -> str:
        if self.i >= len(self.t):
            raise self.err("unexpected end of file")
        tok = self.t[self.i][1]
        self.i += 1
        return tok

    def expect(self, text: str) -> None:
        line = self.line()
        got = self.peek()
        if got != text:
            raise self.err(f"expected {text!r}, found {got!r}", line)
        self.i += 1

    def ident(self) -> str:
        if self.kind() != "id":
            raise self.err(f"expected identifier, found {self.peek()!r}")
        return self.next()

    def integer(self) -> int:
        neg = False
        if self.peek() in ("-", "+"):
            neg = self.next() == "-"
        if self.kind() != "num":
            raise self.err(f"expected integer, found {self.peek()!r}")
        text = self.next()
        try:
            val = int(text, 0)
        except ValueError:
            raise self.err(f"not an integer: {text!r}") from None
        return -val if neg else val

    def string(self) -> str:
        if self.kind() != "str":
            raise self.err(f"expected string literal, found {self.peek()!r}")
        return self.next()[1:-1]

    # -- options ---------------------------------------------------------
    def option_name(self) -> str:
        if self.peek() == "(":
            self.next()
            name = "(" + self.ident() + ")"
            self.expect(")")
            if self.kind() == "id" and (self.peek() or "").startswith("."):
                name += self.next()
            return name
        return self.ident()

    def option_value(self) -> str:
        if self.peek() == "{":
            raise self.err("aggregate option values are not supported")
        if self.peek() in ("-", "+"):
            sign = self.next()
            if self.kind() in ("num", "id"):
                return sign + self.next()
            raise self.err("malformed signed option value")
        if self.kind() in ("str", "num", "id"):
            return self.next()
        raise self.err(f"unexpected option value {self.peek()!r}")

    def option_statement(self) -> tuple[str, str]:
        self.expect("option")
        name = self.option_name()
        self.expect("=")
        val = self.option_value()
        self.expect(";")
        return name, val

    def bracket_options(self) -> dict[str, str]:
        opts: dict[str, str] = {}
        if self.peek() != "[":
            return opts
        self.next()
        while True:
            name = self.option_name()
            self.expect("=")
            opts[name] = self.option_value()
            if self.peek() == ",":
                self.next()
                continue
            self.expect("]")
            return opts

    # -- file ------------------------------------------------------------
    def parse_file(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "syntax": "proto2", "package": "", "imports": [], "options": {},
            "messages": [], "enums": [], "services": [], "first_preamble_line": None,
        }
        while self.i < len(self.t):
            tok = self.peek()
            if tok == "syntax":
                if out["first_preamble_line"] is None and not out["messages"] and not out["enums"] and not out["services"]:
                    out["first_preamble_line"] = self.line()
                self.next()
                self.expect("=")
                out["syntax"] = self.string()
                self.expect(";")
                if out["syntax"] not in ("proto2", "proto3"):
                    raise self.err(f"unsupported syntax {out['syntax']!r}")
            elif tok == "package":
                if out["first_preamble_line"] is None and not out["messages"] and not out["enums"] and not out["services"]:
                    out["first_preamble_line"] = self.line()
                self.next()
                out["package"] = self.ident()
                self.expect(";")
            elif tok == "import":
                if out["first_preamble_line"] is None and not out["messages"] and not out["enums"] and not out["services"]:
                    out["first_preamble_line"] = self.line()
                self.next()
                if self.peek() in ("public", "weak"):
                    self.next()
                out["imports"].append(self.string())
                self.expect(";")
            elif tok == "option":
                if out["first_preamble_line"] is None and not out["messages"] and not out["enums"] and not out["services"]:
                    out["first_preamble_line"] = self.line()
                k, v = self.option_statement()
                out["options"][k] = v
            elif tok == "message":
                out["messages"].append(self.message())
            elif tok == "enum":
                out["enums"].append(self.enum())
            elif tok == "service":
                out["services"].append(self.service())
            elif tok == ";":
                self.next()
            else:
                raise self.err(f"unsupported or unexpected top-level token {tok!r}")
        return out

    def service(self) -> dict[str, Any]:
        line = self.line()
        self.expect("service")
        name = self.ident()
        self.expect("{")
        rpcs: list[dict[str, Any]] = []
        while self.peek() != "}":
            if self.peek() == "option":
                self.option_statement()
            elif self.peek() == "rpc":
                self.next()
                rname = self.ident()
                self.expect("(")
                cs = self.peek() == "stream"
                if cs:
                    self.next()
                req = self.ident()
                self.expect(")")
                self.expect("returns")
                self.expect("(")
                ss = self.peek() == "stream"
                if ss:
                    self.next()
                resp = self.ident()
                self.expect(")")
                if self.peek() == "{":
                    self.next()
                    while self.peek() != "}":
                        self.option_statement()
                    self.next()
                else:
                    self.expect(";")
                rpcs.append({"name": rname, "request": req, "response": resp,
                             "client_stream": cs, "server_stream": ss})
            elif self.peek() == ";":
                self.next()
            else:
                raise self.err(f"unexpected token {self.peek()!r} in service")
            if self.i >= len(self.t):
                raise self.err("unexpected end of file in service")
        self.expect("}")
        return {"name": name, "line": line, "rpcs": rpcs}

    # -- enum ------------------------------------------------------------
    def reserved(self, body: dict[str, Any], allow_max: bool) -> None:
        self.expect("reserved")
        if self.kind() == "str":
            while True:
                body["reserved_names"].append(self.string())
                if self.peek() == ",":
                    self.next()
                    continue
                break
        else:
            while True:
                lo = self.integer()
                hi = lo
                if self.peek() == "to":
                    self.next()
                    if self.peek() == "max":
                        self.next()
                        hi = MAX_FIELD_NUMBER if allow_max else 2147483647
                    else:
                        hi = self.integer()
                body["reserved_ranges"].append([lo, hi])
                if self.peek() == ",":
                    self.next()
                    continue
                break
        self.expect(";")

    def enum(self) -> dict[str, Any]:
        line = self.line()
        self.expect("enum")
        name = self.ident()
        self.expect("{")
        e: dict[str, Any] = {"name": name, "line": line, "values": [], "options": {},
                             "reserved_ranges": [], "reserved_names": []}
        while self.peek() != "}":
            tok = self.peek()
            if tok is None:
                raise self.err("unexpected end of file in enum")
            if tok == "option":
                k, v = self.option_statement()
                e["options"][k] = v
            elif tok == "reserved":
                self.reserved(e, False)
            elif tok == ";":
                self.next()
            elif self.kind() == "id":
                vline = self.line()
                vname = self.next()
                self.expect("=")
                num = self.integer()
                opts = self.bracket_options()
                self.expect(";")
                e["values"].append({"name": vname, "number": num, "options": opts, "line": vline})
            else:
                raise self.err(f"unexpected token {tok!r} in enum {name}")
        self.expect("}")
        e["end_line"] = self.t[self.i - 1][2]
        return e

    # -- message ---------------------------------------------------------
    def message(self) -> dict[str, Any]:
        line = self.line()
        self.expect("message")
        name = self.ident()
        return self.message_body(name, line)

    def message_body(self, name: str, line: int) -> dict[str, Any]:
        self.expect("{")
        m: dict[str, Any] = {
            "name": name, "line": line, "fields": [], "messages": [], "enums": [],
            "oneofs": [], "options": {}, "reserved_ranges": [], "reserved_names": [],
            "extension_ranges": [],
        }
        while self.peek() != "}":
            tok = self.peek()
            if tok is None:
                raise self.err(f"unexpected end of file in message {name}")
            if tok == "option":
                k, v = self.option_statement()
                m["options"][k] = v
            elif tok == "message":
                m["messages"].append(self.message())
            elif tok == "enum":
                m["enums"].append(self.enum())
            elif tok == "reserved":
                self.reserved(m, True)
            elif tok == "extensions":
                self.next()
                while True:
                    lo = self.integer()
                    hi = lo
                    if self.peek() == "to":
                        self.next()
                        if self.peek() == "max":
                            self.next()
                            hi = MAX_FIELD_NUMBER
                        else:
                            hi = self.integer()
                    m["extension_ranges"].append([lo, hi])
                    if self.peek() == ",":
                        self.next()
                        continue
                    break
                self.expect(";")
            elif tok == "oneof":
                self.next()
                oname = self.ident()
                self.expect("{")
                members: list[str] = []
                while self.peek() != "}":
                    if self.peek() == "option":
                        self.option_statement()
                        continue
                    if self.peek() is None:
                        raise self.err("unexpected end of file in oneof")
                    f = self.field(label=None)
                    f["oneof"] = oname
                    m["fields"].append(f)
                    members.append(f["name"])
                self.expect("}")
                m["oneofs"].append({"name": oname, "members": members})
            elif tok == "extend":
                raise self.err("'extend' is not supported by this parser")
            elif tok == "group":
                raise self.err("'group' is not supported by this parser")
            elif tok == ";":
                self.next()
            elif tok in ("required", "optional", "repeated"):
                label = self.next()
                if self.peek() == "group":
                    raise self.err("'group' fields are not supported by this parser")
                m["fields"].append(self.field(label=label))
            else:
                m["fields"].append(self.field(label=None))
        self.expect("}")
        m["end_line"] = self.t[self.i - 1][2]
        return m

    def field(self, label: str | None) -> dict[str, Any]:
        line = self.line()
        if self.peek() == "map":
            self.next()
            self.expect("<")
            ktype = self.ident()
            self.expect(",")
            vtype = self.ident()
            self.expect(">")
            ftype = f"map<{ktype},{vtype}>"
            label = "repeated"
        else:
            ftype = self.ident()
        fname = self.ident()
        self.expect("=")
        number = self.integer()
        if not 1 <= number <= MAX_FIELD_NUMBER:
            raise self.err(f"field number {number} out of range", line)
        opts = self.bracket_options()
        self.expect(";")
        return {"label": label, "type": ftype, "name": fname, "number": number,
                "options": opts, "line": line, "oneof": None}


def parse_proto(content: str, path: str) -> dict[str, Any]:
    return _Parser(tokenize(content, path), path).parse_file()


# ---------------------------------------------------------------------------
# Structural model
# ---------------------------------------------------------------------------

def _collect(
    scope: str, items_m: list[dict[str, Any]], items_e: list[dict[str, Any]],
    rel_prefix: str, file_path: str, syntax: str, msgs: dict[str, dict[str, Any]],
    enums: dict[str, dict[str, Any]],
) -> None:
    for e in items_e:
        fqn = f"{scope}.{e['name']}" if scope else e["name"]
        rel = f"{rel_prefix}{e['name']}"
        if fqn in msgs or fqn in enums:
            raise ProtoParseError(file_path, e["line"], f"duplicate symbol {fqn}")
        enums[fqn] = {
            "fqn": fqn, "rel": rel, "file": file_path, "line": e["line"], "end_line": e["end_line"],
            "syntax": syntax,
            "values": [{"name": v["name"], "number": v["number"], "line": v["line"],
                        "deprecated": v["options"].get("deprecated") == "true"}
                       for v in e["values"]],
            "reserved_ranges": e["reserved_ranges"], "reserved_names": e["reserved_names"],
            "allow_alias": e["options"].get("allow_alias") == "true",
        }
    for m in items_m:
        fqn = f"{scope}.{m['name']}" if scope else m["name"]
        rel = f"{rel_prefix}{m['name']}"
        if fqn in msgs or fqn in enums:
            raise ProtoParseError(file_path, m["line"], f"duplicate symbol {fqn}")
        msgs[fqn] = {
            "fqn": fqn, "rel": rel, "file": file_path, "line": m["line"], "end_line": m["end_line"],
            "syntax": syntax,
            "raw_fields": m["fields"], "oneofs": m["oneofs"],
            "reserved_ranges": m["reserved_ranges"], "reserved_names": m["reserved_names"],
            "extension_ranges": m["extension_ranges"],
            "nested_messages": [f"{fqn}.{x['name']}" for x in m["messages"]],
            "nested_enums": [f"{fqn}.{x['name']}" for x in m["enums"]],
        }
        _collect(fqn, m["messages"], m["enums"], rel + ".", file_path, syntax, msgs, enums)


def _resolve(type_name: str, scope: str, symbols: dict[str, str]) -> str | None:
    if type_name.startswith("."):
        return type_name[1:] if type_name[1:] in symbols else None
    parts = scope.split(".") if scope else []
    while True:
        cand = ".".join(parts + [type_name])
        if cand in symbols:
            return cand
        if not parts:
            return None
        parts.pop()


def _preceding_comment_lines(cinfo: dict[str, Any], start_line: int) -> list[int]:
    """Return sorted line numbers of the contiguous comment block directly above start_line."""
    comment_lines = cinfo["comment_lines"]
    block_start_by_end = cinfo["block_start_by_end"]
    code_lines = cinfo["code_lines"]
    collected: list[int] = []
    cur = start_line - 1
    while cur >= 1 and cur in comment_lines and cur not in code_lines:
        blk_start = block_start_by_end.get(cur, cur)
        if any(ln in code_lines for ln in range(blk_start, cur + 1)):
            break
        collected.extend(range(cur, blk_start - 1, -1))
        cur = blk_start - 1
    collected.reverse()
    return collected


def build_schema_set(
    files: dict[str, str], file_paths: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Parse ``{path: text}`` into a resolved structural model.

    Returns ``{"files": {path: {...}}, "messages": {fqn: ...}, "enums": {fqn: ...}}``.
    Field types are resolved to message/enum/scalar; an unresolvable type name is
    a ``ProtoParseError`` naming file and line.
    """
    parsed: dict[str, dict[str, Any]] = {}
    msgs: dict[str, dict[str, Any]] = {}
    enums: dict[str, dict[str, Any]] = {}
    for path in sorted(files):
        p = parse_proto(files[path], path)
        parsed[path] = p
        _collect(p["package"], p["messages"], p["enums"], "", path, p["syntax"], msgs, enums)
    symbols: dict[str, str] = {k: "message" for k in msgs}
    symbols.update({k: "enum" for k in enums})
    for fqn in sorted(msgs):
        m = msgs[fqn]
        fields: list[dict[str, Any]] = []
        seen_numbers: dict[int, str] = {}
        for rf in m.pop("raw_fields"):
            t = rf["type"]
            num = rf["number"]
            if num in seen_numbers:
                raise ProtoParseError(m["file"], rf["line"],
                                      f"duplicate field number {num} in {fqn}")
            seen_numbers[num] = rf["name"]
            if t.startswith("map<"):
                tkind, resolved, wire = "map", t, "len"
            elif t in SCALAR_WIRE:
                tkind, resolved, wire = "scalar", t, SCALAR_WIRE[t]
            else:
                r = _resolve(t, fqn, symbols)
                if r is None:
                    raise ProtoParseError(m["file"], rf["line"],
                                          f"unresolved type {t!r} in {fqn}.{rf['name']}")
                tkind = symbols[r]
                resolved = r
                wire = "varint" if tkind == "enum" else "len"
            syntax = m["syntax"]
            if rf["label"] == "repeated":
                card = "repeated"
            else:
                card = "singular"
            if rf["label"] == "required":
                presence = "required"
            elif rf["label"] == "optional":
                presence = "optional"
            elif rf["label"] is None and rf["oneof"] is None and syntax == "proto3":
                presence = "implicit"
            elif rf["label"] is None:
                presence = "optional"  # oneof member: explicit presence
            else:
                presence = "repeated"
            packed_opt = rf["options"].get("packed")
            scalar_packable = (tkind == "enum" or (tkind == "scalar" and resolved in PACKABLE))
            if card == "repeated" and scalar_packable:
                packed = (packed_opt == "true") if packed_opt is not None else (syntax == "proto3")
            else:
                packed = False
            fields.append({
                "number": num, "name": rf["name"], "cardinality": card, "presence": presence,
                "type": resolved, "type_kind": tkind, "wire": wire, "packed": packed,
                "default": rf["options"].get("default"),
                "deprecated": rf["options"].get("deprecated") == "true",
                "oneof": rf["oneof"], "line": rf["line"],
            })
        fields.sort(key=lambda f: f["number"])
        m["fields"] = fields
        for rr in m["reserved_ranges"]:
            for f in fields:
                if rr[0] <= f["number"] <= rr[1]:
                    raise ProtoParseError(m["file"], f["line"],
                                          f"field {f['number']} of {fqn} is reserved")
    for e in enums.values():
        e["values"].sort(key=lambda v: (v["number"], v["name"]))

    comments_by_file = {
        path: extract_file_comments(files[path], (file_paths or {}).get(path, path))
        for path in parsed
    }
    preceding_lines_by_ent: dict[tuple[str, str], list[int]] = {}
    for kind, col in (("message", msgs), ("enum", enums)):
        for fqn, ent in col.items():
            preceding_lines_by_ent[(kind, fqn)] = _preceding_comment_lines(
                comments_by_file[ent["file"]], ent["line"],
            )

    for kind, col in (("message", msgs), ("enum", enums)):
        for fqn, ent in col.items():
            fpath = ent["file"]
            cinfo = comments_by_file[fpath]
            comment_lines = cinfo["comment_lines"]
            p = parsed[fpath]
            first_preamble = p["first_preamble_line"]
            top_level_count = len(p["messages"]) + len(p["enums"])

            seen_line_nums: set[int] = set()
            entries: list[tuple[int, str, str]] = []

            def add_line(ln: int, scope_name: str) -> None:
                if ln in seen_line_nums:
                    return
                lines_at = comment_lines.get(ln)
                if not lines_at:
                    return
                seen_line_nums.add(ln)
                for s in lines_at:
                    if s:
                        entries.append((ln, scope_name, s))

            # 1. Contiguous comment block directly above this entity declaration
            for ln in preceding_lines_by_ent[(kind, fqn)]:
                add_line(ln, "entity_comment")

            # 2. Inline/body comments inside this entity's span, excluding nested entities
            excluded: set[int] = set()
            if kind == "message":
                for nfqn in ent["nested_messages"]:
                    nm = msgs[nfqn]
                    excluded.update(range(nm["line"], nm["end_line"] + 1))
                    excluded.update(preceding_lines_by_ent[("message", nfqn)])
                for nfqn in ent["nested_enums"]:
                    ne = enums[nfqn]
                    excluded.update(range(ne["line"], ne["end_line"] + 1))
                    excluded.update(preceding_lines_by_ent[("enum", nfqn)])
            for ln in range(ent["line"], ent["end_line"] + 1):
                if ln not in excluded:
                    add_line(ln, "entity_body")

            # 3. File-header comments (strictly before first preamble token) when the file
            #    defines at most two top-level entities and this entity is top-level or wrapper.Enum
            is_top_level = "." not in ent["rel"]
            is_wrapper_enum = False
            if kind == "enum" and "." in ent["rel"]:
                parent_fqn = fqn.rsplit(".", 1)[0]
                pm = msgs.get(parent_fqn)
                if (pm is not None and "." not in pm["rel"]
                        and not pm["fields"] and not pm["nested_messages"]
                        and len(pm["nested_enums"]) == 1):
                    is_wrapper_enum = True
            if first_preamble is not None and top_level_count <= 2 and (is_top_level or is_wrapper_enum):
                for ln in range(1, first_preamble):
                    add_line(ln, "file_header")

            entries.sort(key=lambda item: (item[0], item[1], item[2]))
            prov_path = (file_paths or {}).get(fpath, fpath)
            ent["annotations"] = _parse_comment_entries(entries, prov_path)

    file_info = {
        path: {
            "syntax": p["syntax"],
            "package": p["package"],
            "imports": p["imports"],
            "services": [s["name"] for s in p["services"]],
            "annotations": comments_by_file[path]["file_annotations"],
        }
        for path, p in parsed.items()
    }
    # imports must resolve to a file in the same set (by suffix) - record, report later
    return {"files": file_info, "messages": msgs, "enums": enums}
