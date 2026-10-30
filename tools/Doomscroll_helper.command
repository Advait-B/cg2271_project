#!/bin/bash
# Starts the laptop helper (browser lock, Telegram, Firebase, key/volume forwarding). Leave this window open.
exec python3 "$(cd "$(dirname "$0")" && pwd)/doomscroll_keys.py"
