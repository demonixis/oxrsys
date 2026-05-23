#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


ALLOWED_TYPES = (
    "build",
    "chore",
    "ci",
    "docs",
    "feat",
    "fix",
    "perf",
    "refactor",
    "revert",
    "style",
    "test",
)
COMMIT_HEADER_RE = re.compile(
    r"^(?:"
    + "|".join(ALLOWED_TYPES)
    + r")(?:\([a-z0-9./_-]+\))?(?:!)?: .+$"
)


def normalize_header(header: str) -> str:
    if header.startswith("Merge "):
        return ""
    if header.startswith("fixup! "):
        return header[len("fixup! ") :]
    if header.startswith("squash! "):
        return header[len("squash! ") :]
    return header


def is_valid_header(header: str) -> bool:
    normalized = normalize_header(header)
    if not normalized:
        return True
    return COMMIT_HEADER_RE.match(normalized) is not None


def print_failure(source_label: str, header: str) -> None:
    print(
        f"""Commit message failed Conventional Commits validation ({source_label}):
  {header}

Expected:
  type(scope): summary
  type: summary

Examples:
  feat(runtime): negotiate refresh rate from client
  fix(companion): keep runtime registration status in sync""",
        file=sys.stderr,
    )


def lint_message(source_label: str, header: str) -> int:
    if not is_valid_header(header):
        print_failure(source_label, header)
        return 1
    print(f"==> Commit message passed ({source_label}): {header}")
    return 0


def lint_range(from_ref: str, to_ref: str) -> int:
    if from_ref == to_ref:
        print("==> Commit range is empty; skipping commitlint.")
        return 0

    command = ["git", "log", "--format=%H%x09%s", f"{from_ref}..{to_ref}"]
    result = subprocess.run(command, check=True, capture_output=True, text=True)

    failures = 0
    for line in result.stdout.splitlines():
        if not line:
            continue
        commit_sha, subject = line.split("\t", 1)
        failures |= lint_message(commit_sha, subject)
    return failures


def lint_edit(message_file: str) -> int:
    lines = pathlib.Path(message_file).read_text(encoding="utf-8").splitlines()
    header = lines[0] if lines else ""
    return lint_message(message_file, header)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate Conventional Commit subjects locally and in CI."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    lint_range_parser = subparsers.add_parser("range")
    lint_range_parser.add_argument("from_ref")
    lint_range_parser.add_argument("to_ref")

    lint_edit_parser = subparsers.add_parser("edit")
    lint_edit_parser.add_argument("message_file")

    lint_message_parser = subparsers.add_parser("message")
    lint_message_parser.add_argument("header")

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "range":
        return lint_range(args.from_ref, args.to_ref)
    if args.command == "edit":
        return lint_edit(args.message_file)
    if args.command == "message":
        return lint_message("--message", args.header)
    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    sys.exit(main())
