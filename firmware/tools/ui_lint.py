#!/usr/bin/env python3
"""UI source lint (Plan 4, spec section 12): keeps amber for NEEDS YOU only, and Host command lines in cc_cmd.

    python3 firmware/tools/ui_lint.py firmware/main

Prints one "<file>:<line>: <rule>" per problem and exits 1 if there are any. Rules:
  amber-literal        f4b545 (any case) in ui/** outside ui/ui_tokens.c
  amber-accessor       ui_tk_needs_you_hex( outside ui_comp.c (and its own ui_tokens.c/.h), or
                       ui_needs_you_color( outside ui_comp.c/.h
  exec-outside-cc_cmd  in ui/**: a string literal "claude attach|logs|agents|-p" or one holding /bin/sh,
                       or a cc_link_exec / cc_link_exec_tail call with no cc_cmd_ call before it in the
                       same function body
"""
import os
import re
import sys

UI = "ui"
SOURCE_EXT = (".c", ".h")
AMBER_RE = re.compile(r"f4b545", re.IGNORECASE)
HEX_ACCESSOR_RE = re.compile(r"\bui_tk_needs_you_hex\s*\(")
COLOR_ACCESSOR_RE = re.compile(r"\bui_needs_you_color\s*\(")
EXEC_CALL_RE = re.compile(r"\bcc_link_exec(?:_tail)?\s*\(")
CC_CMD_RE = re.compile(r"\bcc_cmd_\w+\s*\(")
COMMAND_LITERAL_RE = re.compile(r"claude (attach|logs|agents|-p)")

HEX_ACCESSOR_HOMES = {os.path.join(UI, "ui_comp.c"), os.path.join(UI, "ui_tokens.c"), os.path.join(UI, "ui_tokens.h")}
COLOR_ACCESSOR_HOMES = {os.path.join(UI, "ui_comp.c"), os.path.join(UI, "ui_comp.h")}
# Controller ruling C6: the old dashboard calls ui_needs_you_color() until Task 6 deletes it.
COLOR_ACCESSOR_ALLOW = {os.path.join(UI, "ui_dashboard.c")}


def split_source(text):
    """Returns (code, literals): code is text with comments and the insides of string and character
    literals blanked (newlines kept, so offsets and line numbers still match); literals is a list of
    (offset, contents) for each string literal."""
    out = list(text)
    literals = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if c == '"':
                literals.append((i, text[i + 1:j]))
            for k in range(i + 1, min(j, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out), literals


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def body_starts(code):
    """For each offset, where its enclosing top-level brace block opens (-1 outside any)."""
    starts = [-1] * (len(code) + 1)
    depth, start = 0, -1
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}" and depth:
            depth -= 1
        starts[i] = start if depth else -1
    return starts


def lint_file(rel, text):
    problems = set()
    in_ui = rel.split(os.sep, 1)[0] == UI
    if not in_ui and "needs_you_" not in text:
        return []   # outside ui/ only the accessor rule applies (and the generated fonts are large)
    code, literals = split_source(text)

    if in_ui and rel != os.path.join(UI, "ui_tokens.c"):
        for m in AMBER_RE.finditer(text):
            problems.add((line_of(text, m.start()), "amber-literal"))

    if rel not in HEX_ACCESSOR_HOMES:
        for m in HEX_ACCESSOR_RE.finditer(code):
            problems.add((line_of(code, m.start()), "amber-accessor"))
    if rel not in COLOR_ACCESSOR_HOMES and rel not in COLOR_ACCESSOR_ALLOW:
        for m in COLOR_ACCESSOR_RE.finditer(code):
            problems.add((line_of(code, m.start()), "amber-accessor"))

    if in_ui:
        for offset, s in literals:
            if COMMAND_LITERAL_RE.search(s) or "/bin/sh" in s:
                problems.add((line_of(text, offset), "exec-outside-cc_cmd"))
        starts = body_starts(code)
        for m in EXEC_CALL_RE.finditer(code):
            start = starts[m.start()]
            if start < 0 or not CC_CMD_RE.search(code, start, m.start()):
                problems.add((line_of(code, m.start()), "exec-outside-cc_cmd"))
    return [f"{rel}:{line}: {rule}" for line, rule in sorted(problems)]


def lint(main_dir):
    problems = []
    for root, dirs, files in os.walk(main_dir):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith(SOURCE_EXT):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, main_dir)
            with open(path, encoding="utf-8", errors="replace") as f:
                problems += lint_file(rel, f.read())
    return problems


def main(argv):
    if len(argv) != 2:
        print("usage: ui_lint.py <firmware/main>", file=sys.stderr)
        return 2
    problems = lint(argv[1])
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
