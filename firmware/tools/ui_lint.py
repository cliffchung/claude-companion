#!/usr/bin/env python3
"""UI source lint (Plan 4, spec sections 9 and 12): keeps amber for NEEDS YOU only, Host command lines in cc_cmd,
the word "Host" (never "Mac") in what the user sees, and every UI string in the one tr() table.

    python3 firmware/tools/ui_lint.py firmware/main

Prints one "<file>:<line>: <rule>" per problem and exits 1 if there are any. Rules:
  amber-literal        f4b545 (any case) in ui/** outside ui/ui_tokens.c
  amber-accessor       ui_tk_needs_you_hex( outside ui_comp.c (and its own ui_tokens.c/.h), or
                       ui_needs_you_color( outside ui_comp.c/.h
  exec-outside-cc_cmd  in ui/**: a string literal "claude attach|logs|agents|-p" or one holding /bin/sh,
                       or a cc_link_exec / cc_link_exec_tail call with no cc_cmd_ call before it in the
                       same function body
  mac-word             the word "Mac" inside a string literal in ui/**, i18n/i18n.h, companion/cc_link.c,
                       companion/cc_link.h, companion/cc_console.c or sys/checkin.c ("MacBook", mac_lan_ip pass)
  literal-outside-tr   in ui/**: a string literal passed straight to lv_label_set_text(_fmt|_static),
                       ui_toast*, ui_srow*, ui_btn, ui_chip, ui_banner_set, ui_confirm_ex, ui_prompt or
                       ui_picker that holds a letter once its escapes and printf conversions are removed
                       ("", "%s", "%s \xc2\xb7 %d%%" pass; tr(STR_*) is not a literal)
  ui-alias             UI_BG, UI_CARD, UI_CARD_HI, UI_ACCENT, UI_ACCENT2, UI_DANGER, UI_WARN, UI_TEXT,
                       UI_MUTED or ui_button_colored, word-bounded, anywhere under firmware/main
  unused-str           an X(ID, ...) entry in i18n/i18n.h whose STR_ID no code under firmware/main uses
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

MAC_WORD_RE = re.compile(r"\bMac\b")
MAC_WORD_FILES = {os.path.join("i18n", "i18n.h"), os.path.join("companion", "cc_link.c"),
                  os.path.join("companion", "cc_link.h"), os.path.join("companion", "cc_console.c"),
                  os.path.join("sys", "checkin.c")}
TEXT_CALL_RE = re.compile(r"\b(?:lv_label_set_text(?:_fmt|_static)?|ui_toast\w*|ui_srow\w*|ui_btn|ui_chip|"
                          r"ui_banner_set|ui_confirm_ex|ui_prompt|ui_picker)\s*\(")
ESCAPE_RE = re.compile(r"\\(?:x[0-9a-fA-F]+|[0-7]{1,3}|.)")
CONVERSION_RE = re.compile(r"%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGcsp%]")
ALIAS_RE = re.compile(r"\b(?:UI_BG|UI_CARD|UI_CARD_HI|UI_ACCENT|UI_ACCENT2|UI_DANGER|UI_WARN|UI_TEXT|UI_MUTED|"
                      r"ui_button_colored)\b")
I18N_H = os.path.join("i18n", "i18n.h")
STR_ENTRY_RE = re.compile(r"^X\((\w+),", re.MULTILINE)
STR_USE_RE = re.compile(r"\bSTR_(\w+)")

HEX_ACCESSOR_HOMES = {os.path.join(UI, "ui_comp.c"), os.path.join(UI, "ui_tokens.c"), os.path.join(UI, "ui_tokens.h")}
COLOR_ACCESSOR_HOMES = {os.path.join(UI, "ui_comp.c"), os.path.join(UI, "ui_comp.h")}


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


def is_text(s):
    """A literal holds text when a letter is left after its escapes and printf conversions are removed."""
    return re.search(r"[A-Za-z]", CONVERSION_RE.sub("", ESCAPE_RE.sub("", s))) is not None


def call_args(code, open_paren):
    """Offsets (start, end) of a call's argument list: from just after open_paren to its matching ')'."""
    depth, i = 0, open_paren
    while i < len(code):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return open_paren + 1, i
        i += 1
    return open_paren + 1, len(code)


def literal_args(code, literals):
    """Offsets of the text literals passed straight (not inside a nested call) to a text call."""
    found = []
    for m in TEXT_CALL_RE.finditer(code):
        start, end = call_args(code, m.end() - 1)
        for (offset, s) in literals:
            if start <= offset < end:
                inner = code[start:offset]
                cast = inner.rstrip().endswith(")")   # (void *)"sleep-usb": a key for the callback, not text
                if inner.count("(") - inner.count(")") == 0 and not cast and is_text(s):
                    found.append(offset)
    return found


def lint_file(rel, text):
    problems = set()
    in_ui = rel.split(os.sep, 1)[0] == UI
    for m in ALIAS_RE.finditer(text):
        problems.add((line_of(text, m.start()), "ui-alias"))
    mac_scope = in_ui or rel in MAC_WORD_FILES
    if not in_ui and not mac_scope and "needs_you_" not in text:
        return [f"{rel}:{line}: {rule}" for line, rule in sorted(problems)]   # the generated fonts are large
    code, literals = split_source(text)

    if mac_scope:
        for offset, s in literals:
            if MAC_WORD_RE.search(s):
                problems.add((line_of(text, offset), "mac-word"))

    if in_ui and rel != os.path.join(UI, "ui_tokens.c"):
        for m in AMBER_RE.finditer(text):
            problems.add((line_of(text, m.start()), "amber-literal"))

    if rel not in HEX_ACCESSOR_HOMES:
        for m in HEX_ACCESSOR_RE.finditer(code):
            problems.add((line_of(code, m.start()), "amber-accessor"))
    if rel not in COLOR_ACCESSOR_HOMES:
        for m in COLOR_ACCESSOR_RE.finditer(code):
            problems.add((line_of(code, m.start()), "amber-accessor"))

    if in_ui:
        for offset in literal_args(code, literals):
            problems.add((line_of(text, offset), "literal-outside-tr"))
        for offset, s in literals:
            if COMMAND_LITERAL_RE.search(s) or "/bin/sh" in s:
                problems.add((line_of(text, offset), "exec-outside-cc_cmd"))
        starts = body_starts(code)
        for m in EXEC_CALL_RE.finditer(code):
            start = starts[m.start()]
            if start < 0 or not CC_CMD_RE.search(code, start, m.start()):
                problems.add((line_of(code, m.start()), "exec-outside-cc_cmd"))
    return [f"{rel}:{line}: {rule}" for line, rule in sorted(problems)]


def unused_strings(texts):
    """unused-str: the i18n.h entries whose STR_ id no code (comments and literals aside) uses."""
    table = texts.get(I18N_H)
    if table is None:
        return []
    used = set()
    for rel, text in texts.items():
        if rel == I18N_H or "STR_" not in text:
            continue
        used.update(STR_USE_RE.findall(split_source(text)[0]))
    return [f"{I18N_H}:{line_of(table, m.start())}: unused-str"
            for m in STR_ENTRY_RE.finditer(table) if m.group(1) not in used]


def lint(main_dir):
    problems = []
    texts = {}
    for root, dirs, files in os.walk(main_dir):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith(SOURCE_EXT):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, main_dir)
            with open(path, encoding="utf-8", errors="replace") as f:
                texts[rel] = f.read()
            problems += lint_file(rel, texts[rel])
    return problems + unused_strings(texts)


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
