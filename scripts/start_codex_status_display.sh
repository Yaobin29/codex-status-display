#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARGS=("$@")

if [[ -n "${CODEX_STATUS_ROOT:-}" ]]; then
  ARGS=(--root "$CODEX_STATUS_ROOT" "${ARGS[@]}")
fi

exec python3 "$ROOT/codex_status_display.py" "${ARGS[@]}"
