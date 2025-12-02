#!/bin/bash
cd "$(dirname "$0")/../viewer"
source venv/bin/activate
python3 audio_test.py "$@"
