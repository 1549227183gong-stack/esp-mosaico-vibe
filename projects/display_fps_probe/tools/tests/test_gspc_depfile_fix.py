#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gspc depfile 修复逻辑的单元测试。

运行方式（在工程根目录）：

    python -m unittest discover -s tools/tests -v
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import gspc_depfile_fix as fixer


class FindDepfileArgumentTest(unittest.TestCase):
    """命令行 --depfile 解析。"""

    def test_space_form(self) -> None:
        self.assertEqual(
            fixer.find_depfile_argument(["compile", "--depfile", "a.d"]),
            "a.d")

    def test_equals_form(self) -> None:
        self.assertEqual(
            fixer.find_depfile_argument(["--depfile=b.d", "x"]), "b.d")

    def test_missing(self) -> None:
        self.assertIsNone(fixer.find_depfile_argument(["--version"]))


class NormalizeDepfileTextTest(unittest.TestCase):
    """depfile 文本规范化。"""

    def test_escaped_drive_path(self) -> None:
        raw = r"\\\\?\\E:\\dir\\a.ttf: \\\\?\\E:\\dir\\b.ttf" + "\n"
        fixed, changes = fixer.normalize_depfile_text(raw)
        self.assertEqual(fixed, "E:/dir/a.ttf: E:/dir/b.ttf\n")
        self.assertGreater(changes, 0)

    def test_escaped_unc_path(self) -> None:
        raw = r"\\\\?\\UNC\\server\\share\\a.ttf"
        fixed, _ = fixer.normalize_depfile_text(raw)
        self.assertEqual(fixed, "//server/share/a.ttf")

    def test_unescaped_windows_path(self) -> None:
        raw = r"\\?\E:\dir\a.ttf"
        fixed, _ = fixer.normalize_depfile_text(raw)
        self.assertEqual(fixed, "E:/dir/a.ttf")

    def test_clean_path_is_idempotent(self) -> None:
        raw = "E:/dir/a.ttf"
        fixed, changes = fixer.normalize_depfile_text(raw)
        self.assertEqual(fixed, raw)
        self.assertEqual(changes, 0)

    def test_escaped_space_is_preserved(self) -> None:
        raw = r"E:\\dir\\my\ file.ttf"
        fixed, _ = fixer.normalize_depfile_text(raw)
        self.assertEqual(fixed, r"E:/dir/my\ file.ttf")

    def test_second_pass_changes_nothing(self) -> None:
        raw = r"\\\\?\\E:\\dir\\a.ttf"
        once, _ = fixer.normalize_depfile_text(raw)
        twice, changes = fixer.normalize_depfile_text(once)
        self.assertEqual(once, twice)
        self.assertEqual(changes, 0)


if __name__ == "__main__":
    unittest.main()
