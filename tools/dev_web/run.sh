#!/bin/bash
# Launch the Saturn dev dashboard on pi5-jenkins.
# Open http://<host>:8089/ in a browser.
#
# Tip: to keep it running across logout, use:
#   nohup ./tools/dev_web/run.sh > /tmp/dev_web.log 2>&1 &
set -e
cd "$(dirname "$0")/../.."
exec python3 tools/dev_web/server.py "$@"
