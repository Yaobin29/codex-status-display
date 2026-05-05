#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import sqlite3
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path
from typing import Any


DEFAULT_SERIAL = "/dev/cu.usbmodem111201"
STATE_DIR_NAME = "codex-status-display"
WIRE_SCHEMA_VERSION = 1
DEFAULT_MAX_WIRE_BYTES = 1024
DEFAULT_PROJECT_WHITELIST = {
    "PaperBanana": ("projects/PaperBanana", "paperbanana"),
    "digital-twin-chip": ("projects/digital-twin-chip", "digital-twin-chip"),
}

TASK_RE = re.compile(r"^- \[(?P<mark>[ xX])\] (?P<title>.+)$")
SECTION_RE = re.compile(r"^##+\s+(?P<title>.+?)\s*$")
DONE_APPROVAL_STATUSES = {"done", "resolved", "closed", "approved", "rejected", "dismissed"}
COMPLETED_STATUSES = {"success"}
COMPLETED_LOOKBACK_HOURS = 24
CHAT_RUNNING_STALE_MINUTES = 120
THREAD_SCAN_LIMIT = 500
MAX_ROLLOUT_TAIL_BYTES = 1_000_000
CODEX_STATE_DB = "state_5.sqlite"
THREAD_TERMINAL_EVENTS = {"task_complete", "turn_aborted", "error"}
APPROVAL_EVENT_HINTS = ("approval", "approve", "permission_request")
AWAITING_RESPONSE_TOOL_NAMES = {"request_user_input"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a compact Codex/Pandora status snapshot for a USB display board.",
    )
    parser.add_argument("--root", help="Avatar Node repo root. Defaults to auto-detect from cwd.")
    parser.add_argument("--now", help="ISO timestamp override, mainly for tests.")
    parser.add_argument("--serial", help=f"Serial device to write to, for example {DEFAULT_SERIAL}.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate. Default: 115200.")
    parser.add_argument("--watch", action="store_true", help="Keep refreshing instead of running once.")
    parser.add_argument("--interval", type=float, default=5.0, help="Refresh interval in seconds for --watch.")
    parser.add_argument("--dry-run", action="store_true", help="Print the wire payload but do not write serial.")
    parser.add_argument("--quiet", action="store_true", help="Do not print the wire payload to stdout.")
    parser.add_argument("--max-wire-bytes", type=int, default=DEFAULT_MAX_WIRE_BYTES)
    parser.add_argument("--project-name", action="append", default=[], help="Extra whitelisted project name to scan for.")
    parser.add_argument("--mark-seen", action="store_true", help="Legacy: mark current completed chat runs as viewed and exit.")
    parser.add_argument("--http", action="store_true", help="Serve the wire payload over HTTP for WiFi display mode.")
    parser.add_argument("--http-host", default="0.0.0.0", help="HTTP bind host for --http. Default: 0.0.0.0.")
    parser.add_argument("--http-port", type=int, default=8787, help="HTTP port for --http. Default: 8787.")
    return parser.parse_args()


def candidate_roots(explicit_root: str | None) -> list[Path]:
    candidates: list[Path] = []
    if explicit_root:
        candidates.append(Path(explicit_root).expanduser())
    for env_name in ("PANDORA_NODE_ROOT", "ROBIN_AI_ROOT"):
        raw = os.environ.get(env_name, "").strip()
        if raw:
            candidates.append(Path(raw).expanduser())
    here = Path.cwd().resolve()
    candidates.extend([here, *here.parents])
    candidates.extend(Path(__file__).resolve().parents)
    return candidates


def resolve_project_root(explicit_root: str | None = None) -> Path:
    seen: set[Path] = set()
    for candidate in candidate_roots(explicit_root):
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if (resolved / "AGENTS.md").exists() and (resolved / "local-runtime").exists():
            return resolved
    raise SystemExit("Could not locate the Avatar Node root. Pass --root explicitly.")


def parse_now(raw: str | None) -> dt.datetime:
    if raw:
        return dt.datetime.fromisoformat(raw)
    return dt.datetime.now().astimezone()


def load_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return default
    return json.loads(text)


def write_json(path: Path, payload: Any, *, ascii_only: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=ascii_only, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def state_dir(root: Path) -> Path:
    return root / "local-runtime" / STATE_DIR_NAME


def state_paths(root: Path) -> dict[str, Path]:
    base = state_dir(root)
    return {
        "base": base,
        "status": base / "status.json",
        "manual_approvals": base / "manual-approvals.json",
        "seen_completions": base / "seen-completions.json",
        "seen_chat_completions": base / "seen-chat-completions.json",
        "log": base / "status.log.jsonl",
    }


def ensure_state(root: Path) -> dict[str, Path]:
    paths = state_paths(root)
    paths["base"].mkdir(parents=True, exist_ok=True)
    if not paths["manual_approvals"].exists():
        write_json(paths["manual_approvals"], {"approvals": []})
    if not paths["seen_completions"].exists():
        write_json(paths["seen_completions"], {"seen": []})
    if not paths["seen_chat_completions"].exists():
        write_json(paths["seen_chat_completions"], {"seen": []})
    if not paths["log"].exists():
        paths["log"].write_text("", encoding="utf-8")
    return paths


def append_rolling_log(path: Path, record: dict[str, Any], max_lines: int = 200) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(record, ensure_ascii=False, sort_keys=True)
    existing = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    lines = [*existing, line][-max_lines:]
    path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def safe_text(value: Any, limit: int = 72) -> str:
    text = str(value or "").strip()
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"`([^`]+)`", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    if len(text) <= limit:
        return text
    return text[: max(0, limit - 1)].rstrip() + "..."


def current_task_list_path(root: Path, now: dt.datetime) -> Path | None:
    preferred = root / "local-runtime" / "task-list" / now.strftime("%Y-%m") / f"task-list-{now.strftime('%Y-%m')}.md"
    if preferred.exists():
        return preferred
    candidates = sorted((root / "local-runtime" / "task-list").glob("*/task-list-*.md"))
    return candidates[-1] if candidates else None


def collect_tasks(root: Path, now: dt.datetime) -> dict[str, Any]:
    path = current_task_list_path(root, now)
    open_tasks: list[dict[str, str]] = []
    done_count = 0
    current_section = ""
    if path and path.exists():
        for raw in path.read_text(encoding="utf-8").splitlines():
            section_match = SECTION_RE.match(raw)
            if section_match:
                current_section = safe_text(section_match.group("title"), 40)
                continue
            match = TASK_RE.match(raw)
            if not match:
                continue
            title = safe_text(match.group("title"), 92)
            if match.group("mark").lower() == "x":
                done_count += 1
            else:
                open_tasks.append({"title": title, "section": current_section or "Tasks"})

    urgent = [item for item in open_tasks if is_urgent_section(item["section"])]
    return {
        "path": str(path) if path else None,
        "open": open_tasks,
        "open_count": len(open_tasks),
        "urgent_count": len(urgent),
        "done_count": done_count,
        "top_urgent": urgent[:4],
        "top_open": open_tasks[:6],
    }


def is_urgent_section(section: str) -> bool:
    lower = section.lower()
    if "不紧急" in section or "not urgent" in lower:
        return False
    return "紧急" in section or "urgent" in lower


def parse_datetime(value: Any) -> dt.datetime | None:
    text = str(value or "").strip()
    if not text:
        return None
    try:
        parsed = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed


def parse_jsonl_timestamp(value: Any) -> dt.datetime | None:
    text = str(value or "").strip()
    if not text:
        return None
    return parse_datetime(text.replace("Z", "+00:00"))


def codex_home() -> Path:
    raw = os.environ.get("CODEX_HOME", "").strip()
    return Path(raw).expanduser() if raw else Path.home() / ".codex"


def load_seen_keys(path: Path) -> set[str]:
    payload = load_json(path, {"seen": []})
    raw_seen = payload.get("seen") if isinstance(payload, dict) else payload
    if not isinstance(raw_seen, list):
        return set()
    return {str(item) for item in raw_seen if str(item).strip()}


def collect_automations(root: Path) -> dict[str, Any]:
    state_root = root / "local-runtime" / "automations"
    items: list[dict[str, Any]] = []
    blockers: list[dict[str, str]] = []
    if not state_root.exists():
        return {"items": [], "blockers": []}

    for path in sorted(state_root.glob("*/last-run.json")):
        payload = load_json(path, {})
        if not isinstance(payload, dict) or not payload:
            continue
        automation_id = safe_text(payload.get("automation_id") or path.parent.name, 48)
        display_name = safe_text(payload.get("display_name") or automation_id, 48)
        status = safe_text(payload.get("status") or "unknown", 24)
        completed_at = safe_text(payload.get("completed_at") or "", 32)
        raw_blockers = payload.get("blockers") or []
        if isinstance(raw_blockers, str):
            raw_blockers = [raw_blockers]
        clean_blockers = [safe_text(item, 80) for item in raw_blockers if str(item).strip()]
        summary = safe_text(payload.get("summary") or "", 96)
        item = {
            "id": automation_id,
            "name": display_name,
            "status": status,
            "completed_at": completed_at,
            "blockers": clean_blockers,
            "summary": summary,
        }
        items.append(item)
        status_lower = status.lower()
        has_blocker_state = status_lower in {"blocked", "failed", "failure", "error"} or (
            clean_blockers and status_lower not in {"success", "noop"}
        )
        if has_blocker_state:
            reason = clean_blockers[0] if clean_blockers else f"status={status}"
            blockers.append({"name": display_name, "reason": reason})
    return {"items": items, "blockers": blockers}


def completion_key(item: dict[str, Any]) -> str:
    return f"{item.get('id', '')}|{item.get('completed_at', '')}"


def load_seen_completion_keys(root: Path) -> set[str]:
    paths = ensure_state(root)
    payload = load_json(paths["seen_completions"], {"seen": []})
    raw_seen = payload.get("seen") if isinstance(payload, dict) else payload
    if not isinstance(raw_seen, list):
        return set()
    return {str(item) for item in raw_seen if str(item).strip()}


def completed_runs(automations: dict[str, Any], now: dt.datetime, root: Path) -> list[dict[str, str]]:
    seen = load_seen_completion_keys(root)
    cutoff = now - dt.timedelta(hours=COMPLETED_LOOKBACK_HOURS)
    runs: list[tuple[dt.datetime, dict[str, str]]] = []
    for item in automations.get("items", []):
        if not isinstance(item, dict):
            continue
        if str(item.get("status") or "").lower() not in COMPLETED_STATUSES:
            continue
        completed_at = parse_datetime(item.get("completed_at"))
        if not completed_at:
            continue
        comparable_now = now
        if comparable_now.tzinfo is None:
            comparable_now = comparable_now.replace(tzinfo=completed_at.tzinfo or dt.timezone.utc)
        if completed_at.tzinfo is None:
            completed_at = completed_at.replace(tzinfo=comparable_now.tzinfo or dt.timezone.utc)
        comparable_cutoff = cutoff
        if comparable_cutoff.tzinfo is None:
            comparable_cutoff = comparable_cutoff.replace(tzinfo=completed_at.tzinfo or dt.timezone.utc)
        if completed_at < comparable_cutoff:
            continue
        key = completion_key(item)
        if key in seen:
            continue
        runs.append(
            (
                completed_at,
                {
                    "id": safe_text(item.get("id"), 48),
                    "title": safe_text(item.get("name") or item.get("id") or "completed", 48),
                    "status": "done",
                    "completed_at": safe_text(item.get("completed_at"), 32),
                    "summary": safe_text(item.get("summary"), 64),
                    "key": key,
                },
            )
        )
    runs.sort(key=lambda pair: pair[0], reverse=True)
    return [item for _, item in runs]


def mark_completed_seen(root: Path, automations: dict[str, Any], now: dt.datetime) -> int:
    paths = ensure_state(root)
    existing = load_seen_completion_keys(root)
    visible = completed_runs(automations, now, root)
    for item in visible:
        key = str(item.get("key") or "")
        if key:
            existing.add(key)
    write_json(paths["seen_completions"], {"seen": sorted(existing)})
    return len(visible)


def thread_display_name(row: dict[str, Any]) -> str:
    nickname = safe_text(row.get("agent_nickname"), 24)
    title = safe_text(row.get("title") or row.get("id") or "chat", 44)
    if nickname:
        return nickname
    return title


def thread_rows_from_codex(limit: int = THREAD_SCAN_LIMIT) -> list[dict[str, Any]]:
    db_path = codex_home() / CODEX_STATE_DB
    if not db_path.exists():
        return []
    query = """
        select id, title, rollout_path, updated_at, archived, agent_nickname, agent_role
        from threads
        where archived = 0
        order by updated_at desc
        limit ?
    """
    try:
        con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        con.row_factory = sqlite3.Row
        with con:
            return [dict(row) for row in con.execute(query, (limit,))]
    except sqlite3.Error:
        return []


def thread_event_summary(rollout_path: Any) -> dict[str, Any]:
    path = Path(str(rollout_path or "")).expanduser()
    summary: dict[str, Any] = {
        "last_status_event": None,
        "last_status_at": None,
        "last_turn_id": "",
        "last_completion_at": None,
        "pending_approval": False,
        "pending_response": False,
        "pending_response_at": None,
    }
    if not path.exists():
        return summary
    pending_response_calls: dict[str, dt.datetime | None] = {}
    for line in recent_jsonl_lines(path):
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        record_type = str(record.get("type") or "")
        payload = record.get("payload")
        if not isinstance(payload, dict):
            continue
        event_at = parse_jsonl_timestamp(record.get("timestamp"))
        if record_type == "event_msg":
            event_type = str(payload.get("type") or "")
            turn_id = str(payload.get("turn_id") or payload.get("turnId") or "")
            if event_type == "task_started":
                pending_response_calls.clear()
                summary.update(
                    {
                        "last_status_event": "task_started",
                        "last_status_at": event_at,
                        "last_turn_id": turn_id,
                        "pending_approval": False,
                    }
                )
            elif event_type == "task_complete":
                pending_response_calls.clear()
                summary.update(
                    {
                        "last_status_event": "task_complete",
                        "last_status_at": event_at,
                        "last_completion_at": event_at,
                        "last_turn_id": turn_id,
                        "pending_approval": False,
                    }
                )
            elif event_type in THREAD_TERMINAL_EVENTS:
                pending_response_calls.clear()
                summary.update(
                    {
                        "last_status_event": event_type,
                        "last_status_at": event_at,
                        "last_turn_id": turn_id,
                        "pending_approval": False,
                    }
                )
            elif any(hint in event_type.lower() for hint in APPROVAL_EVENT_HINTS):
                summary.update(
                    {
                        "last_status_event": event_type,
                        "last_status_at": event_at,
                        "last_turn_id": turn_id,
                        "pending_approval": True,
                    }
                )
        elif record_type == "response_item":
            item_type = str(payload.get("type") or "")
            if item_type == "function_call" and str(payload.get("name") or "") in AWAITING_RESPONSE_TOOL_NAMES:
                call_id = str(payload.get("call_id") or "")
                if call_id:
                    pending_response_calls[call_id] = event_at
            elif item_type == "function_call_output":
                call_id = str(payload.get("call_id") or "")
                pending_response_calls.pop(call_id, None)
    pending_response_at = max(
        (when for when in pending_response_calls.values() if isinstance(when, dt.datetime)),
        default=None,
    )
    summary["pending_response"] = bool(pending_response_calls)
    summary["pending_response_at"] = pending_response_at
    return summary


def normalize_now_for_compare(now: dt.datetime) -> dt.datetime:
    return now if now.tzinfo else now.replace(tzinfo=dt.timezone.utc)


def is_same_local_day(value: dt.datetime, now: dt.datetime) -> bool:
    comparable_now = normalize_now_for_compare(now)
    comparable_value = value
    if comparable_value.tzinfo is None:
        comparable_value = comparable_value.replace(tzinfo=comparable_now.tzinfo or dt.timezone.utc)
    else:
        comparable_value = comparable_value.astimezone(comparable_now.tzinfo or dt.timezone.utc)
    return comparable_value.date() == comparable_now.date()


def chat_completion_key(thread_id: str, completed_at: dt.datetime | None) -> str:
    return f"{thread_id}|{completed_at.isoformat() if completed_at else ''}"


def recent_jsonl_lines(path: Path, max_bytes: int = MAX_ROLLOUT_TAIL_BYTES) -> list[str]:
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            if size > max_bytes:
                handle.seek(size - max_bytes)
                handle.readline()
            data = handle.read()
    except OSError:
        return []
    return data.decode("utf-8", errors="ignore").splitlines()


def collect_chat_threads(root: Path, now: dt.datetime) -> dict[str, Any]:
    ensure_state(root)
    comparable_now = normalize_now_for_compare(now)
    running_cutoff = comparable_now - dt.timedelta(minutes=CHAT_RUNNING_STALE_MINUTES)

    running: list[dict[str, str]] = []
    completed_today: list[dict[str, str]] = []
    awaiting_response: list[dict[str, str]] = []
    active_projects: list[dict[str, str]] = []

    for row in thread_rows_from_codex():
        thread_id = str(row.get("id") or "")
        if not thread_id:
            continue
        events = thread_event_summary(row.get("rollout_path"))
        title = thread_display_name(row)
        status_event = events.get("last_status_event")
        status_at = events.get("last_status_at")
        if events.get("pending_response"):
            pending_at = events.get("pending_response_at")
            waiting_item = {
                "title": title,
                "status": "awaiting_response",
                "source": "codex-chat",
                "id": thread_id,
            }
            if isinstance(pending_at, dt.datetime):
                waiting_item["waiting_since"] = pending_at.isoformat()
            awaiting_response.append(waiting_item)
            active_projects.append({"name": title, "status": "awaiting_response", "id": thread_id})
            continue
        if status_event == "task_started" and isinstance(status_at, dt.datetime) and status_at >= running_cutoff:
            running_item = {"name": title, "status": "running", "id": thread_id}
            running.append(running_item)
            active_projects.append(running_item)
            continue
        completed_at = events.get("last_completion_at")
        if isinstance(completed_at, dt.datetime) and is_same_local_day(completed_at, now):
            key = chat_completion_key(thread_id, completed_at)
            completed_today.append(
                {
                    "title": title,
                    "status": "done",
                    "completed_at": completed_at.isoformat(),
                    "key": key,
                    "id": thread_id,
                }
            )

    return {
        "running": running[:8],
        "active_projects": active_projects[:8],
        "completed_today": completed_today[:24],
        "completed_unseen": completed_today[:24],
        "awaiting_response": awaiting_response[:8],
        "source": str(codex_home() / CODEX_STATE_DB),
    }


def mark_chat_completed_seen(root: Path, now: dt.datetime) -> int:
    paths = ensure_state(root)
    existing = load_seen_keys(paths["seen_chat_completions"])
    chat = collect_chat_threads(root, now)
    for item in chat["completed_today"]:
        key = str(item.get("key") or "")
        if key:
            existing.add(key)
    write_json(paths["seen_chat_completions"], {"seen": sorted(existing)})
    return len(chat["completed_today"])


def collect_promote_reviews(root: Path) -> dict[str, Any]:
    path = root / "local-runtime" / "automations" / "v2-outputs-eywa-promote" / "latest-candidates.json"
    payload = load_json(path, {})
    if not isinstance(payload, dict):
        return {"path": str(path), "needs_review_count": 0, "items": []}
    candidates = payload.get("candidates") or []
    if not isinstance(candidates, list):
        candidates = []
    review_items: list[dict[str, str]] = []
    for item in candidates:
        if not isinstance(item, dict):
            continue
        decision = str(item.get("decision") or item.get("status") or "").strip()
        if decision != "needs_review":
            continue
        source = safe_text(item.get("rel_source") or item.get("source") or "review item", 68)
        reason = safe_text(item.get("reason") or "needs review", 84)
        review_items.append({"source": source, "reason": reason})
    return {
        "path": str(path),
        "needs_review_count": len(review_items),
        "items": review_items[:6],
    }


def collect_manual_approvals(root: Path) -> list[dict[str, str]]:
    paths = ensure_state(root)
    payload = load_json(paths["manual_approvals"], {"approvals": []})
    raw_items = payload.get("approvals") if isinstance(payload, dict) else payload
    if not isinstance(raw_items, list):
        return []

    approvals: list[dict[str, str]] = []
    for raw in raw_items:
        if isinstance(raw, str):
            title = safe_text(raw, 72)
            if title:
                approvals.append({"title": title, "source": "manual", "status": "open"})
            continue
        if not isinstance(raw, dict):
            continue
        status = safe_text(raw.get("status") or "open", 20).lower()
        if status in DONE_APPROVAL_STATUSES:
            continue
        title = safe_text(raw.get("title") or raw.get("summary") or raw.get("task") or "", 72)
        if not title:
            continue
        approvals.append(
            {
                "title": title,
                "source": safe_text(raw.get("source") or "manual", 36),
                "status": status,
            }
        )
    return approvals[:6]


def project_whitelist(extra_names: list[str]) -> dict[str, tuple[str, ...]]:
    whitelist = dict(DEFAULT_PROJECT_WHITELIST)
    for name in extra_names:
        clean = safe_text(name, 48)
        if clean:
            whitelist[clean] = (clean,)
    return whitelist


def collect_running_projects(extra_names: list[str] | None = None) -> list[dict[str, str]]:
    whitelist = project_whitelist(extra_names or [])
    try:
        output = subprocess.check_output(["ps", "-axo", "command"], text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []

    running: list[dict[str, str]] = []
    for name, tokens in whitelist.items():
        token_lowers = [token.lower() for token in tokens]
        found = False
        for command in output.splitlines():
            lower = command.lower()
            if "codex_status_display.py" in lower:
                continue
            if any(token in lower for token in token_lowers):
                found = True
                break
        if found:
            running.append({"name": name, "status": "running"})
    return running


def build_snapshot(root: Path, now: dt.datetime, extra_project_names: list[str] | None = None) -> dict[str, Any]:
    chat = collect_chat_threads(root, now)

    alerts: list[str] = []
    for item in chat["awaiting_response"][:2]:
        alerts.append(safe_text(f"Response needed: {item['title']}", 74))
    for item in chat["running"][:2]:
        alerts.append(safe_text(f"Running: {item['name']}", 74))
    for item in chat["completed_today"][:2]:
        alerts.append(safe_text(f"Done: {item['title']}", 74))

    counts = {
        "awaiting_response": len(chat["awaiting_response"]),
        "running_projects": len(chat["active_projects"]),
        "completed_today": len(chat["completed_today"]),
        "done_unseen": len(chat["completed_today"]),
    }
    return {
        "schema_version": WIRE_SCHEMA_VERSION,
        "updated_at": now.isoformat(),
        "counts": counts,
        "alerts": alerts[:6],
        "projects": chat["active_projects"][:6],
        "awaiting": chat["awaiting_response"][:6],
        "completed": chat["completed_today"][:6],
        "sources": {
            "codex_threads": chat["source"],
            "seen_chat_completions": str(state_paths(root)["seen_chat_completions"]),
            "completed_day": normalize_now_for_compare(now).date().isoformat(),
        },
    }


def wire_payload(snapshot: dict[str, Any], max_bytes: int) -> dict[str, Any]:
    limits = [(4, 4, 4, 2), (3, 3, 3, 1), (2, 2, 2, 1), (1, 1, 1, 0), (0, 0, 0, 0)]
    base = {
        "v": WIRE_SCHEMA_VERSION,
        "t": snapshot["updated_at"],
        "counts": snapshot["counts"],
    }
    for project_limit, awaiting_limit, completed_limit, alert_limit in limits:
        payload = {
            **base,
            "projects": compact_items(snapshot.get("projects"), ("name", "status"), project_limit),
            "awaiting": compact_items(snapshot.get("awaiting"), ("title", "status", "waiting_since"), awaiting_limit),
            "completed": compact_items(snapshot.get("completed"), ("title", "status", "completed_at"), completed_limit),
            "alerts": snapshot.get("alerts", [])[:alert_limit],
        }
        if len(encode_wire_line(payload)) <= max_bytes:
            return payload
    return {"v": WIRE_SCHEMA_VERSION, "t": snapshot["updated_at"], "counts": snapshot["counts"]}


def encode_wire_line(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode("ascii")


def compact_items(items: Any, fields: tuple[str, ...], limit: int) -> list[dict[str, str]]:
    if not isinstance(items, list):
        return []
    compacted: list[dict[str, str]] = []
    for item in items[:limit]:
        if not isinstance(item, dict):
            continue
        compacted.append({field: safe_text(item.get(field), 56) for field in fields if str(item.get(field) or "").strip()})
    return compacted


def render_wire_line(snapshot: dict[str, Any], max_bytes: int = DEFAULT_MAX_WIRE_BYTES) -> str:
    payload = wire_payload(snapshot, max_bytes)
    encoded = encode_wire_line(payload)
    if len(encoded) > max_bytes:
        raise ValueError(f"Wire payload is {len(encoded)} bytes, over the {max_bytes} byte limit")
    return encoded.decode("ascii")


def cached_snapshot(root: Path, now: dt.datetime, max_age_seconds: float) -> dict[str, Any] | None:
    payload = load_json(state_paths(root)["status"], {})
    if not isinstance(payload, dict) or not payload:
        return None
    updated_at = parse_datetime(payload.get("updated_at"))
    if not updated_at:
        return None
    comparable_now = normalize_now_for_compare(now)
    if updated_at.tzinfo is None:
        updated_at = updated_at.replace(tzinfo=comparable_now.tzinfo or dt.timezone.utc)
    if comparable_now - updated_at > dt.timedelta(seconds=max_age_seconds):
        return None
    return payload


def fresh_or_built_snapshot(root: Path, now: dt.datetime, args: argparse.Namespace) -> dict[str, Any]:
    max_age = max(float(getattr(args, "interval", 10.0)) * 2.5, 15.0)
    snapshot = cached_snapshot(root, now, max_age)
    if snapshot is not None:
        return snapshot
    snapshot = build_snapshot(root, now, args.project_name)
    write_json(state_paths(root)["status"], snapshot)
    return snapshot


def build_and_persist_line(root: Path, now: dt.datetime, args: argparse.Namespace, *, serial_label: str, dry_run: bool) -> str:
    paths = ensure_state(root)
    snapshot = build_snapshot(root, now, args.project_name)
    write_json(paths["status"], snapshot)
    line = render_wire_line(snapshot, args.max_wire_bytes)
    append_rolling_log(
        paths["log"],
        {
            "updated_at": snapshot["updated_at"],
            "counts": snapshot["counts"],
            "wire_bytes": len(line.encode("ascii")),
            "serial": serial_label,
            "dry_run": dry_run,
        },
    )
    return line


class StatusHTTPHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path not in {"/", "/wire", "/status", "/health"}:
            self.send_error(404, "not found")
            return
        root: Path = self.server.root  # type: ignore[attr-defined]
        args: argparse.Namespace = self.server.args  # type: ignore[attr-defined]
        if self.path == "/health":
            self.send_json_line({"ok": True})
            return
        now = parse_now(args.now)
        if self.path == "/status":
            snapshot = fresh_or_built_snapshot(root, now, args)
            self.send_json_line(snapshot, ascii_only=False)
            return
        snapshot = fresh_or_built_snapshot(root, now, args)
        line = render_wire_line(snapshot, args.max_wire_bytes)
        append_rolling_log(
            state_paths(root)["log"],
            {
                "updated_at": snapshot["updated_at"],
                "counts": snapshot["counts"],
                "wire_bytes": len(line.encode("ascii")),
                "serial": "http",
                "dry_run": False,
            },
        )
        self.send_bytes(line.encode("ascii") + b"\n", "application/json; charset=ascii")

    def log_message(self, fmt: str, *args: Any) -> None:
        server_args: argparse.Namespace = self.server.args  # type: ignore[attr-defined]
        if not server_args.quiet:
            super().log_message(fmt, *args)

    def send_json_line(self, payload: dict[str, Any], *, ascii_only: bool = True) -> None:
        body = json.dumps(payload, ensure_ascii=ascii_only, separators=(",", ":"), sort_keys=True).encode("utf-8") + b"\n"
        self.send_bytes(body, "application/json; charset=utf-8")

    def send_bytes(self, body: bytes, content_type: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_http_server(root: Path, args: argparse.Namespace) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((args.http_host, args.http_port), StatusHTTPHandler)
    server.root = root  # type: ignore[attr-defined]
    server.args = args  # type: ignore[attr-defined]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    if not args.quiet:
        print(f"http_status=http://{args.http_host}:{server.server_port}/wire", flush=True)
    return server


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    if not hasattr(termios, name):
        raise ValueError(f"Unsupported baud rate for termios: {baud}")
    return int(getattr(termios, name))


def configure_serial_fd(fd: int, baud: int = 115200) -> None:
    attrs = termios.tcgetattr(fd)
    speed = baud_constant(baud)
    attrs[4] = speed
    attrs[5] = speed
    attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    attrs[1] &= ~termios.OPOST
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ISIG)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_serial_device(device: str, baud: int = 115200) -> int:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure_serial_fd(fd, baud)
    return fd


def write_serial_fd(fd: int, line: str) -> None:
    os.write(fd, line.encode("ascii") + b"\n")
    termios.tcdrain(fd)


def write_serial_line(device: str, line: str, baud: int = 115200) -> None:
    fd = open_serial_device(device, baud)
    try:
        write_serial_fd(fd, line)
    finally:
        os.close(fd)


def run_once(root: Path, now: dt.datetime, args: argparse.Namespace, serial_fd: int | None = None) -> str:
    line = build_and_persist_line(root, now, args, serial_label=args.serial or "", dry_run=bool(args.dry_run or not args.serial))
    if args.serial and not args.dry_run:
        if serial_fd is not None:
            write_serial_fd(serial_fd, line)
        else:
            write_serial_line(args.serial, line, args.baud)
    if not args.quiet:
        print(line, flush=True)
    return line


def main() -> int:
    args = parse_args()
    root = resolve_project_root(args.root)
    if args.mark_seen:
        now = parse_now(args.now)
        marked = mark_chat_completed_seen(root, now)
        if not args.quiet:
            print(f"marked_seen={marked}", flush=True)
        return 0
    serial_fd: int | None = None
    httpd: ThreadingHTTPServer | None = None
    try:
        if args.http:
            httpd = start_http_server(root, args)
        if args.watch and args.serial and not args.dry_run:
            serial_fd = open_serial_device(args.serial, args.baud)
        if args.http and not args.watch and not args.serial and not args.dry_run:
            while True:
                time.sleep(3600)
        while True:
            now = parse_now(args.now)
            run_once(root, now, args, serial_fd)
            if not args.watch:
                return 0
            time.sleep(max(args.interval, 1.0))
    finally:
        if serial_fd is not None:
            os.close(serial_fd)
        if httpd is not None:
            httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
