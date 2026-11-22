import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import live_quality


SPLIT_ANSWER = (
    "导出默认文件名定义在：\n\n"
    "- 文件：`names.py`\n"
    "- 行号：第 1 行\n"
    "- 文件名：`report.json`\n\n"
    "定义代码为：`EXPORT_NAME = 'report.json'`。`app.py:4` 使用该常量生成导出路径。"
)


class CitationTest(unittest.TestCase):
    def test_inline_citation(self):
        self.assertEqual(live_quality.source_citation("See `names.py:1`.", "names.py", 1), "path:line")
        self.assertEqual(live_quality.source_citation("`./app.py:2-3`", "app.py", 2), "path:line")

    def test_adjacent_labeled_fields(self):
        self.assertEqual(live_quality.source_citation(SPLIT_ANSWER, "names.py", 1), "separate fields")
        english = "- **File:** `./names.py`\r\n- **Line number:** 1"
        self.assertEqual(live_quality.source_citation(english, "names.py", 1), "separate fields")

    def test_file_and_line_must_refer_to_same_location(self):
        for answer in (
            "other/names.py:1",
            "other-names.py:1",
            "names.py:10",
            "names.py:1.5",
            "- 文件：`names.py`\n- 行号：第 10 行",
            "- 文件：`other/names.py`\n- 行号：第 1 行",
            "- 文件：`names.py`\n- 文件：`app.py`\n- 行号：第 1 行",
            "`names.py` defines the value.\n\nLine: 1",
        ):
            with self.subTest(answer=answer):
                self.assertIsNone(live_quality.source_citation(answer, "names.py", 1))

    def test_attempt_reports_location_and_format_independently(self):
        case = next(case for case in json.loads(live_quality.CATALOG.read_text(encoding="utf-8"))
                    if case["id"] == "locate-output-name")
        args = SimpleNamespace(cli=Path("lay-code"), model="fixture", provider=None, provider_config=None)
        with TemporaryDirectory() as directory, patch.object(live_quality, "prepare"), \
                patch.object(live_quality.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            for answer, passed, formatted in (
                (SPLIT_ANSWER, True, False),
                ("`names.py:1`", True, True),
                ("`names.py:10`", False, False),
            ):
                with self.subTest(answer=answer):
                    history = {"finished": True, "rounds": [{"text": answer, "tools": []}]}
                    with patch.object(live_quality, "context", return_value=history):
                        attempt = live_quality.run_case(args, case, 1, Path(directory))
                    self.assertEqual((attempt["passed"], attempt["citation_format_ok"]), (passed, formatted))


if __name__ == "__main__":
    unittest.main()
