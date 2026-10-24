#!/bin/sh
#
# frank-quake2 — uses tools/git-hooks as this clone's git hooks: commit
# messages lose AI-tool trailers, AI-tool files and game data are refused.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")/.."
chmod +x tools/git-hooks/*
git config core.hooksPath tools/git-hooks
echo "git hooks: $(git config core.hooksPath)"
