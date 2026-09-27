#!/usr/bin/env python3
"""Release notes: one line per commit since the previous v* tag.

  python tools/release_notes.py [owner/name]

Prints a markdown list, oldest commit first, each subject linked to its
commit. The repository defaults to GITHUB_REPOSITORY. The first release has
no previous tag and starts after the fork from cspot_vita. CI runs it on the
tag.
"""
import os
import re
import subprocess
import sys

# Last commit of michal4132/cspot_vita before this fork's own history.
FORK_BASE = "e6044b4"


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout.strip()


def release_notes(repository):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository or ""):
        raise SystemExit("expected a GitHub repository in owner/name form")
    try:
        # Starting at the parent leaves out the release tag on HEAD.
        previous = git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", "HEAD^")
    except subprocess.CalledProcessError:
        previous = FORK_BASE
    log = git("log", "--reverse", "--format=%H%x09%s", f"{previous}..HEAD")
    lines = []
    for line in filter(None, log.split("\n")):
        sha, subject = line.split("\t", 1)
        subject = re.sub(r"([\\`*_{}\[\]<>()!|])", r"\\\1", subject)
        lines.append(f"- {subject} ([{sha[:8]}](https://github.com/{repository}/commit/{sha}))")
    return "\n".join(lines)


if __name__ == "__main__":
    print(release_notes(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GITHUB_REPOSITORY")))
