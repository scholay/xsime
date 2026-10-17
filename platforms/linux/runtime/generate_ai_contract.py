#!/usr/bin/env python3
"""Import the plain AI API request contract from the native macOS builder."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (ROOT / 'Sources/RimeBuffer/AITextPlugins.swift').read_text()
builder = source.split('enum AITextOpenAIRequestBuilder {', 1)[1]
match = re.search(r'case \.semanticBlocks:\s*systemPrompt = ("(?:\\.|[^"\\])*")', builder)
if not match:
    raise SystemExit('Review the macOS AI API request contract before building Linux')
prompt = json.loads(match.group(1))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('#pragma once\nnamespace rimes::native {\ninline constexpr const char* aiSystemPrompt = '
                       + json.dumps(prompt) + ';\n}\n')
