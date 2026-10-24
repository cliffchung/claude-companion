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


if __name__ == "__main__":
    unittest.main()
