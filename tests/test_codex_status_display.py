from __future__ import annotations

import datetime as dt
import importlib.util
import json
import os
import sqlite3
import tempfile
import unittest
import urllib.request
from pathlib import Path
from types import SimpleNamespace
from typing import Any


MODULE_PATH = Path(__file__).resolve().parents[1] / "codex_status_display.py"
SPEC = importlib.util.spec_from_file_location("codex_status_display", MODULE_PATH)
assert SPEC and SPEC.loader
status_display = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(status_display)


class CodexStatusDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "local-runtime").mkdir()
        (self.root / "AGENTS.md").write_text("test\n", encoding="utf-8")
        self.now = dt.datetime(2026, 5, 2, 9, 30, tzinfo=dt.timezone.utc)
        self.old_codex_home = os.environ.get("CODEX_HOME")
        self.codex_home = self.root / "codex-home"
        self.codex_home.mkdir()
        os.environ["CODEX_HOME"] = str(self.codex_home)
        self.addCleanup(self.restore_env)

    def write(self, rel: str, text: str) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def restore_env(self) -> None:
        if self.old_codex_home is None:
            os.environ.pop("CODEX_HOME", None)
        else:
            os.environ["CODEX_HOME"] = self.old_codex_home

    def write_codex_thread(
        self,
        thread_id: str,
        title: str,
        events: list[tuple[str, dt.datetime] | tuple[str, dt.datetime, dict[str, Any]]],
        *,
        archived: int = 0,
        nickname: str | None = None,
    ) -> None:
        db = self.codex_home / "state_5.sqlite"
        rollout = self.codex_home / f"{thread_id}.jsonl"
        records: list[str] = []
        for idx, item in enumerate(events):
            event_type, when = item[0], item[1]
            extra = item[2] if len(item) > 2 else {}
            if event_type.startswith("response_item:"):
                payload = {"type": event_type.split(":", 1)[1], **extra}
                record_type = "response_item"
            else:
                payload = {"type": event_type, "turn_id": f"{thread_id}-{idx}", **extra}
                record_type = "event_msg"
            records.append(
                json.dumps(
                    {
                        "timestamp": when.isoformat().replace("+00:00", "Z"),
                        "type": record_type,
                        "payload": payload,
                    }
                )
            )
        rollout.write_text("\n".join(records) + "\n", encoding="utf-8")
        with sqlite3.connect(db) as con:
            con.execute(
                """
                create table if not exists threads (
                    id text primary key,
                    title text,
                    rollout_path text,
                    updated_at integer,
                    archived integer,
                    agent_nickname text,
                    agent_role text
                )
                """
            )
            con.execute(
                """
                insert or replace into threads
                (id, title, rollout_path, updated_at, archived, agent_nickname, agent_role)
                values (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    thread_id,
                    title,
                    str(rollout),
                    int(events[-1][1].timestamp()),
                    archived,
                    nickname,
                    None,
                ),
            )

    def test_snapshot_ignores_old_task_review_and_blocker_counts(self) -> None:
        self.write(
            "local-runtime/task-list/2026-05/task-list-2026-05.md",
            """# 任务清单

## 紧急重要

- [ ] Handle reviewer invitation
- [x] Done item

## 重要不紧急

- [ ] Prepare soft robotics brainstorm
""",
        )
        self.write(
            "local-runtime/automations/v2-demo/last-run.json",
            json.dumps(
                {
                    "automation_id": "v2-demo",
                    "display_name": "Demo",
                    "status": "blocked",
                    "completed_at": self.now.isoformat(),
                    "blockers": ["Needs user approval"],
                }
            ),
        )
        self.write(
            "local-runtime/automations/v2-outputs-eywa-promote/latest-candidates.json",
            json.dumps(
                {
                    "candidates": [
                        {"decision": "needs_review", "rel_source": "outputs/research/demo.csv", "reason": "CSV needs review"},
                        {"decision": "rejected", "rel_source": "outputs/skip.json"},
                    ]
                }
            ),
        )

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(
            snapshot["counts"],
            {"awaiting_response": 0, "completed_today": 0, "done_unseen": 0, "running_projects": 0},
        )
        self.assertNotIn("open_tasks", snapshot["counts"])
        self.assertNotIn("automation_blockers", snapshot["counts"])
        self.assertFalse(any("Blocked:" in item for item in snapshot["alerts"]))

    def test_manual_approvals_do_not_count_as_awaiting_response(self) -> None:
        self.write(
            "local-runtime/codex-status-display/manual-approvals.json",
            json.dumps({"approvals": [{"title": "Approve local run", "source": "manual", "status": "open"}]}),
        )

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(snapshot["counts"]["awaiting_response"], 0)
        self.assertEqual(snapshot["awaiting"], [])

    def test_today_completed_chats_count_until_local_midnight(self) -> None:
        self.write_codex_thread(
            "thread-done",
            "Finished Demo",
            [("task_started", self.now - dt.timedelta(minutes=4)), ("task_complete", self.now - dt.timedelta(minutes=3))],
        )
        self.write_codex_thread(
            "thread-yesterday",
            "Yesterday Demo",
            [
                ("task_started", self.now - dt.timedelta(days=1, minutes=4)),
                ("task_complete", self.now - dt.timedelta(days=1, minutes=3)),
            ],
        )

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(snapshot["counts"]["completed_today"], 1)
        self.assertEqual(snapshot["counts"]["done_unseen"], 1)
        self.assertEqual(snapshot["completed"][0]["title"], "Finished Demo")

        marked = status_display.mark_chat_completed_seen(self.root, self.now)
        self.assertEqual(marked, 1)
        snapshot = status_display.build_snapshot(self.root, self.now)
        self.assertEqual(snapshot["counts"]["completed_today"], 1)
        self.assertEqual(snapshot["counts"]["done_unseen"], 1)

        tomorrow = self.now + dt.timedelta(days=1)
        snapshot = status_display.build_snapshot(self.root, tomorrow)
        self.assertEqual(snapshot["counts"]["completed_today"], 0)
        self.assertEqual(snapshot["counts"]["done_unseen"], 0)

    def test_running_chat_is_counted_from_last_task_started(self) -> None:
        self.write_codex_thread("thread-run", "Active Chat", [("task_started", self.now - dt.timedelta(minutes=5))])

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(snapshot["counts"]["running_projects"], 1)
        self.assertEqual(snapshot["projects"][0]["name"], "Active Chat")

    def test_pending_user_input_counts_as_awaiting_response(self) -> None:
        self.write_codex_thread(
            "thread-await",
            "Needs Choice",
            [
                ("task_started", self.now - dt.timedelta(minutes=5)),
                (
                    "response_item:function_call",
                    self.now - dt.timedelta(minutes=4),
                    {"name": "request_user_input", "call_id": "ask-1"},
                ),
            ],
        )

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(snapshot["counts"]["awaiting_response"], 1)
        self.assertEqual(snapshot["counts"]["running_projects"], 1)
        self.assertEqual(snapshot["projects"][0]["name"], "Needs Choice")
        self.assertEqual(snapshot["projects"][0]["status"], "awaiting_response")
        self.assertEqual(snapshot["awaiting"][0]["title"], "Needs Choice")
        self.assertEqual(snapshot["awaiting"][0]["status"], "awaiting_response")

    def test_answered_user_input_returns_to_running(self) -> None:
        self.write_codex_thread(
            "thread-answered",
            "Answered Choice",
            [
                ("task_started", self.now - dt.timedelta(minutes=5)),
                (
                    "response_item:function_call",
                    self.now - dt.timedelta(minutes=4),
                    {"name": "request_user_input", "call_id": "ask-1"},
                ),
                (
                    "response_item:function_call_output",
                    self.now - dt.timedelta(minutes=3),
                    {"call_id": "ask-1", "output": "{}"},
                ),
            ],
        )

        snapshot = status_display.build_snapshot(self.root, self.now)

        self.assertEqual(snapshot["counts"]["awaiting_response"], 0)
        self.assertEqual(snapshot["counts"]["running_projects"], 1)
        self.assertEqual(snapshot["projects"][0]["name"], "Answered Choice")

    def test_wire_line_is_ascii_json_and_under_limit(self) -> None:
        self.write(
            "local-runtime/task-list/2026-05/task-list-2026-05.md",
            "\n".join(["## 紧急重要", *[f"- [ ] Task {idx}" for idx in range(20)]]) + "\n",
        )
        snapshot = status_display.build_snapshot(self.root, self.now)

        line = status_display.render_wire_line(snapshot, 1024)

        encoded = line.encode("ascii")
        self.assertLessEqual(len(encoded), 1024)
        parsed = json.loads(line)
        self.assertEqual(parsed["v"], 1)
        self.assertEqual(
            parsed["counts"],
            {"awaiting_response": 0, "completed_today": 0, "done_unseen": 0, "running_projects": 0},
        )
        self.assertIn("awaiting", parsed)
        self.assertNotIn("approvals", parsed)

    def test_http_wire_endpoint_serves_compact_payload(self) -> None:
        self.write_codex_thread("thread-run", "Active Chat", [("task_started", self.now - dt.timedelta(minutes=5))])
        args = SimpleNamespace(
            http_host="127.0.0.1",
            http_port=0,
            max_wire_bytes=1024,
            now=self.now.isoformat(),
            project_name=[],
            quiet=True,
        )
        server = status_display.start_http_server(self.root, args)
        self.addCleanup(server.shutdown)
        self.addCleanup(server.server_close)

        with urllib.request.urlopen(f"http://127.0.0.1:{server.server_port}/wire", timeout=5) as response:
            payload = json.loads(response.read().decode("ascii"))

        self.assertEqual(payload["counts"]["running_projects"], 1)
        self.assertEqual(payload["projects"][0]["name"], "Active Chat")

    def test_script_does_not_reference_codex_app_private_storage(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")

        self.assertNotIn("Application Support/Codex", source)
        self.assertNotIn("IndexedDB", source)


if __name__ == "__main__":
    unittest.main()
