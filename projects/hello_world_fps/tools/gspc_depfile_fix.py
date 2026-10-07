#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""修正 gspc 在 Windows 上写出的 depfile 路径。

背景（已在 ESP-IDF 提交 7b9cc1ac79 与 gspc 0.6.1 上复现）：
    gspc 会把 JSON 引用到的资源路径规范化为扩展长度路径，形如
    ``\\\\?\\E:\\dir\\font.ttf``。该路径写进 ``--depfile`` 后：

    1. CMake 的 ``cmake_transform_depfile`` 把它转换成 ``//?/E:/dir/...``；
    2. Ninja 解析后把 ``/E:/dir/...`` 记入 ``.ninja_deps``；
    3. 下一次 Ninja 启动时对该非法路径调用 ``FindFirstFileExA``，直接报
       “The filename, directory name, or volume label syntax is incorrect.”
       并终止构建。

    结果是“第一次构建成功、之后每次增量构建在启动阶段失败”。

用法：
    在 gspc 成功执行之后、CMake 转换 depfile 之前调用，参数与 gspc 完全一致：

        python gspc_depfile_fix.py <gspc 原始参数...>

    depfile 里 Windows 路径按 Makefile 规则做了转义，实际字节形如
    ``\\\\?\\E:\\dir\\font.ttf``。脚本按“先剥掉扩展长度前缀、再把转义
    反斜杠对 ``\\`` 还原为 ``/``”的顺序规范化，保留 ``\ ``（转义空格）
    与行尾续行。没有 ``--depfile`` 参数时（例如 gspc 的 ``--version`` /
    ``compatibility`` 调用）不做任何修改，也不会输出内容。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Sequence

# depfile 中 Windows 路径是转义形式：每个反斜杠写作两个反斜杠。
# 因此扩展长度前缀 \\?\ 在文件里写作 \\\\?\\，UNC 形式写作 \\\\?\\UNC\\。
EXTENDED_ESCAPED = "\\\\\\\\?\\\\"
EXTENDED_UNC_ESCAPED = "\\\\\\\\?\\\\UNC\\\\"
ESCAPED_SEPARATOR = "\\\\"

# 未转义形式（防御性兼容）：\\?\ 与 UNC 形式 \\?\UNC\
EXTENDED_PREFIX = "\\\\?\\"
EXTENDED_UNC_PREFIX = "\\\\?\\UNC\\"

# 兼容未转义的 Windows 路径：反斜杠后面跟空格或换行时属于 depfile 转义，必须保留
LEFTOVER_SEPARATOR = re.compile(r"\\(?![ \t\r\n])")


def find_depfile_argument(arguments: Sequence[str]) -> str | None:
    """从 gspc 命令行中取出 --depfile 的目标路径，未指定时返回 None。"""
    for index, argument in enumerate(arguments):
        if argument == "--depfile" and index + 1 < len(arguments):
            return arguments[index + 1]
        if argument.startswith("--depfile="):
            return argument.split("=", 1)[1]
    return None


def normalize_depfile_text(text: str) -> tuple[str, int]:
    """返回规范化后的 depfile 文本与改动次数。"""
    changes = 0
    # UNC 前缀更长，必须先于盘符前缀处理，避免被短模式抢先匹配
    if EXTENDED_UNC_ESCAPED in text:
        changes += text.count(EXTENDED_UNC_ESCAPED)
        text = text.replace(EXTENDED_UNC_ESCAPED, "//")
    if EXTENDED_ESCAPED in text:
        changes += text.count(EXTENDED_ESCAPED)
        text = text.replace(EXTENDED_ESCAPED, "")
    if EXTENDED_UNC_PREFIX in text:
        changes += text.count(EXTENDED_UNC_PREFIX)
        text = text.replace(EXTENDED_UNC_PREFIX, "//")
    if EXTENDED_PREFIX in text:
        changes += text.count(EXTENDED_PREFIX)
        text = text.replace(EXTENDED_PREFIX, "")
    # 转义反斜杠对还原为路径分隔符，转义空格与行尾续行不受影响
    if ESCAPED_SEPARATOR in text:
        changes += text.count(ESCAPED_SEPARATOR)
        text = text.replace(ESCAPED_SEPARATOR, "/")
    text, replaced = LEFTOVER_SEPARATOR.subn("/", text)
    return text, changes + replaced


def main(argv: Sequence[str]) -> int:
    depfile_argument = find_depfile_argument(argv)
    if not depfile_argument:
        return 0

    depfile = Path(depfile_argument)
    if not depfile.is_file():
        print(f"[gspc-depfile-fix] 找不到 depfile：{depfile}", file=sys.stderr)
        return 1

    with depfile.open("r", encoding="utf-8", errors="surrogateescape",
                      newline="") as handle:
        original = handle.read()
    fixed, changes = normalize_depfile_text(original)
    if not changes or fixed == original:
        return 0

    with depfile.open("w", encoding="utf-8", errors="surrogateescape",
                      newline="") as handle:
        handle.write(fixed)
    print(f"[gspc-depfile-fix] {depfile}：修正 {changes} 处扩展路径/分隔符",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
