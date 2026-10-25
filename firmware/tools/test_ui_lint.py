import os
import tempfile
import unittest

import ui_lint


class LintTests(unittest.TestCase):
    def tree(self, files):
        """A temporary firmware/main with `files` ({relative path: text}); returns its path."""
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        for rel, text in files.items():
            p = os.path.join(d.name, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
        return d.name

    def rules(self, problems):
        return [p.rsplit(": ", 1)[1] for p in problems]

    def test_amber_literal_flagged(self):
        main = self.tree({"ui/x.c": "#include <stdint.h>\nstatic const uint32_t c = 0xF4B545;\n"})
        problems = ui_lint.lint(main)
        self.assertEqual(self.rules(problems), ["amber-literal"])
        self.assertTrue(problems[0].startswith(os.path.join("ui", "x.c") + ":2:"))

    def test_amber_in_tokens_allowed(self):
        main = self.tree({"ui/ui_tokens.c": "uint32_t ui_tk_needs_you_hex(void) { return 0xf4b545; }\n",
                          "ui/ui_tokens.h": "uint32_t ui_tk_needs_you_hex(void);\n"})
        self.assertEqual(ui_lint.lint(main), [])

    def test_accessor_outside_comp_flagged(self):
        main = self.tree({
            "ui/ui_comp.c": "void f(void) { lv_color_t a = ui_needs_you_color(); (void)ui_tk_needs_you_hex(); }\n",
            "ui/ui_comp.h": "lv_color_t ui_needs_you_color(void);\n",
            "ui/z.c": "void g(void) { lv_color_t a = ui_needs_you_color(); }\n",
            "sys/w.c": "unsigned h(void) { return ui_tk_needs_you_hex(); }\n",
        })
        problems = ui_lint.lint(main)
        self.assertEqual(sorted(self.rules(problems)), ["amber-accessor", "amber-accessor"])
        files = sorted(p.split(":", 1)[0] for p in problems)
        self.assertEqual(files, [os.path.join("sys", "w.c"), os.path.join("ui", "z.c")])

    def test_exec_literal_flagged(self):
        main = self.tree({"ui/y.c": "void f(void) {\n    cc_link_exec(\"claude agents\", buf, sizeof(buf));\n}\n"})
        problems = ui_lint.lint(main)
        self.assertIn("exec-outside-cc_cmd", self.rules(problems))
        self.assertTrue(all(p.startswith(os.path.join("ui", "y.c") + ":2:") for p in problems))

    def test_exec_without_cc_cmd_flagged(self):
        main = self.tree({"ui/y.c": "void f(const char *c) {\n    cc_link_exec_tail(c, o, 1, 0, 0, 0, 1);\n}\n"})
        self.assertEqual(self.rules(ui_lint.lint(main)), ["exec-outside-cc_cmd"])

    def test_clean_tree_passes(self):
        main = self.tree({
            "ui/a.c": "/* Start one with `claude --bg` */\n"
                      "static const char *k = \"No sessions. Start one with `claude --bg`.\";\n"
                      "void run(void) {\n    char cmd[64];\n    cc_cmd_logs(cmd, sizeof(cmd), \"x\");\n"
                      "    cc_link_exec_tail(cmd, o, 1, 0, 0, 0, 1);\n}\n",
            "ui/ui_comp.c": "lv_color_t ui_needs_you_color(void) { return lv_color_hex(ui_tk_needs_you_hex()); }\n",
            "sys/power.c": "/* amber means needs you */\n",
        })
        self.assertEqual(ui_lint.lint(main), [])

    def test_mac_word_flagged(self):
        main = self.tree({
            "ui/a.c": "void f(void) {\n    ui_toast(\"Can't reach your Mac\");\n}\n",
            "ui/b.c": "/* the Mac's fallback */\nvoid g(void) { s.mac_lan_ip[0] = 0; h(\"MacBook\"); h(\"mac.local\"); }\n",
            "companion/cc_console.c": "static const char *k = \"Mac entry host\";\n",
            "companion/other.c": "static const char *k = \"Mac\";\n",
            "i18n/i18n.h": "X(A, \"No Mac set up\") \\\n",
        })
        problems = ui_lint.lint(main)
        mac = sorted(p for p in problems if p.endswith("mac-word"))
        self.assertEqual(mac, [os.path.join("companion", "cc_console.c") + ":1: mac-word",
                               os.path.join("i18n", "i18n.h") + ":1: mac-word",
                               os.path.join("ui", "a.c") + ":2: mac-word"])

    def test_literal_outside_tr_flagged(self):
        main = self.tree({"ui/a.c": "void f(lv_obj_t *l) {\n"
                                    "    lv_label_set_text(l, \"Loading\");\n"
                                    "    ui_toast_ms(\"Saved\", 900);\n"
                                    "    ui_srow_value(r, \"3 hosts\");\n"
                                    "    ui_confirm_ex(\"Leave?\", tr(STR_OK), cb, NULL);\n"
                                    "    ui_prompt(tr(STR_A), \"box.local\", cur, false, cb, NULL);\n"
                                    "    lv_label_set_text_fmt(l, \"%d hosts\", n);\n"
                                    "}\n"})
        problems = ui_lint.lint(main)
        self.assertEqual(self.rules(problems), ["literal-outside-tr"] * 6)
        self.assertEqual([int(p.split(":")[1]) for p in problems], [2, 3, 4, 5, 6, 7])

    def test_tr_and_format_allowed(self):
        main = self.tree({"ui/a.c": "void f(lv_obj_t *l) {\n"
                                    "    lv_label_set_text(l, tr(STR_DONE));\n"
                                    "    lv_label_set_text(l, \"\");\n"
                                    "    lv_label_set_text_fmt(l, \"%s\", x);\n"
                                    "    lv_label_set_text_fmt(l, \"%s \\xc2\\xb7 %d%%\", x, n);\n"
                                    "    ui_toast(tr(STR_SAVED));\n"
                                    "    ui_chip(p, LV_SYMBOL_LEFT, cb, NULL);\n"
                                    "    lv_label_set_text(l, s.name);\n"
                                    "    ui_srow(p, UI_ROW_SWITCH, tr(STR_A), on, cb, (void *)\"sleep-usb\");\n"
                                    "}\n",
                          "i18n/i18n.h": "X(DONE, \"Done\") \\\nX(SAVED, \"Saved\") \\\nX(A, \"A\")\n",
                          "ui/b.c": "int x = STR_A;\n"})
        self.assertEqual(ui_lint.lint(main), [])

    def test_ui_alias_flagged(self):
        main = self.tree({
            "ui/a.c": "void f(lv_obj_t *o) {\n    lv_obj_set_style_bg_color(o, UI_CARD, 0);\n"
                      "    lv_obj_set_style_bg_color(o, lv_color_hex(UI_TK_PANEL), 0);\n}\n",
            "sys/b.c": "lv_obj_t *b = ui_button_colored(p, t, c, cb, NULL);\n",
            "ui/c.h": "#define UI_MUTED_X 1\n#define UI_TEXT lv_color_hex(UI_TK_FG)\n",
        })
        problems = ui_lint.lint(main)
        self.assertEqual(sorted(problems), [os.path.join("sys", "b.c") + ":1: ui-alias",
                                            os.path.join("ui", "a.c") + ":2: ui-alias",
                                            os.path.join("ui", "c.h") + ":2: ui-alias"])

    def test_unused_str_flagged(self):
        main = self.tree({"i18n/i18n.h": "#define STR_TABLE(X) \\\nX(USED, \"Used\") \\\nX(DEAD, \"Dead\")\n",
                          "ui/a.c": "const char *s = tr(STR_USED);\n"})
        self.assertEqual(ui_lint.lint(main), [os.path.join("i18n", "i18n.h") + ":3: unused-str"])

    def test_literal_rule_covers_more_calls_macros_and_casts(self):
        main = self.tree({"ui/a.c": "#define HELLO \"Hello there\"\n"
                                    "void f(lv_obj_t *p) {\n"
                                    "    ui_header(p, \"Sessions\", NULL);\n"
                                    "    ui_text(p, \"Hi\", UI_TONE_FG, false);\n"
                                    "    ui_alert(\"Oops\", NULL);\n"
                                    "    ui_empty_state(p, \"Nothing here\", NULL, NULL, NULL);\n"
                                    "    lv_textarea_set_placeholder_text(p, \"Type\");\n"
                                    "    ui_toast((const char *)\"Text\");\n"
                                    "    ui_toast(HELLO);\n"
                                    "    ui_toast(HELLO_ID);\n"
                                    "}\n"})
        problems = ui_lint.lint(main)
        self.assertEqual(self.rules(problems), ["literal-outside-tr"] * 7)
        self.assertEqual([int(p.split(":")[1]) for p in problems], [3, 4, 5, 6, 7, 8, 9])

    def test_macro_from_a_header_flagged(self):
        main = self.tree({"ui/a.h": "#define GREETING \"Welcome\"\n#define KEY \"%s\"\n",
                          "ui/b.c": "void f(void) {\n    ui_toast(GREETING);\n    ui_toast(KEY);\n}\n"})
        self.assertEqual(ui_lint.lint(main), [os.path.join("ui", "b.c") + ":2: literal-outside-tr"])

    def test_mac_plural_and_possessive_flagged(self):
        main = self.tree({"ui/a.c": "const char *a = \"Your Macs\";\nconst char *b = \"the Mac's key\";\n"
                                    "const char *c = \"MacBook Pro\";\n"})
        self.assertEqual(self.rules(ui_lint.lint(main)), ["mac-word", "mac-word"])


    # I2 (final review): the Tailscale event callback reaches ui_async (the LVGL lock), and the LVGL task takes s_ctl.
    TS = os.path.join("net", "tailscale.c")

    def test_notify_under_ctl_flagged(self):
        main = self.tree({self.TS: (
            "static void notify(void) {}\n"
            "static bool start_locked(void)\n{\n    notify();\n    return true;\n}\n"
            "void a(void)\n{\n    ctl_lock();\n    notify();\n    ctl_unlock();\n}\n")})
        problems = ui_lint.lint(main)
        self.assertEqual(self.rules(problems), ["notify-under-lock", "notify-under-lock"])
        self.assertTrue(problems[0].startswith(self.TS + ":4:"))
        self.assertTrue(problems[1].startswith(self.TS + ":10:"))

    def test_notify_after_unlock_passes(self):
        main = self.tree({self.TS: (
            "void a(void)\n{\n    ctl_lock();\n    bool d = x;\n    ctl_unlock();\n    if (d) notify();\n}\n"
            "/* notify() in a comment */\n"
            "static void notify_later(void) { s = true; }\n"
            "static bool start_locked(void) { notify_later(); return true; }\n")})
        self.assertEqual(ui_lint.lint(main), [])

    def test_notify_after_an_early_unlock_return_still_locked(self):
        # "if (x) { ctl_unlock(); return; }" leaves the lock held on the path that goes on
        main = self.tree({self.TS: (
            "void a(void)\n{\n    ctl_lock();\n    if (x) { ctl_unlock(); return; }\n    notify();\n    ctl_unlock();\n"
            "    notify();\n}\n")})
        problems = ui_lint.lint(main)
        self.assertEqual(self.rules(problems), ["notify-under-lock"])
        self.assertTrue(problems[0].startswith(self.TS + ":5:"))

    # P5-7 (Plan 5): the whisper command line and its defaults live in cc_cmd only.
    def test_whisper_literal_flagged(self):
        main = self.tree({
            "ui/ui_sessions.c": "static const char *w = \"/x/whisper-cli\";\n",
            "companion/cc_cmd.h": "#define CC_WHISPER_DEFAULT \"/x/whisper-cli\"\n",
            "companion/cc_cmd.c": "static const char *f = \"exec \\\"$W\\\" -otxt -of -\";\n",
            "i18n/i18n.h": "X(VOICE_NO_WHISPER, \"/x/whisper-cli isn't on the Host\") \\\n",
            "voice/voice.c": "/* runs whisper-cli on the Host */\nstatic int n;\n",
        })
        problems = [p for p in ui_lint.lint(main) if not p.endswith("unused-str")]
        self.assertEqual(problems, [os.path.join("ui", "ui_sessions.c") + ":1: whisper-outside-cc_cmd"])

    def test_otxt_literal_flagged(self):
        main = self.tree({"voice/voice.c": "void f(void) {\n    run(\"w -otxt -of -\");\n}\n",
                          "companion/cc_link.c": "static const char *a = \"x\", *b = \"-otxt\";\n"})
        problems = ui_lint.lint(main)
        self.assertEqual(sorted(problems), [os.path.join("companion", "cc_link.c") + ":1: whisper-outside-cc_cmd",
                                            os.path.join("voice", "voice.c") + ":2: whisper-outside-cc_cmd"])


if __name__ == "__main__":
    unittest.main()
