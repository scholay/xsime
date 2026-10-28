#!/usr/bin/env bash
# Only authenticated immutable artifacts enter the draft-and-publish transaction.
# This never builds, signs, runs packages, or replaces an existing asset.
set -euo pipefail
exec python3 "$(dirname "$0")/publish_update_assets.py"
