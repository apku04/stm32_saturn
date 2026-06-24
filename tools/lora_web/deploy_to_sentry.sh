#!/usr/bin/env bash
#
# Deploy the LoRa web dashboard onto the gateway (Pi Zero 2 "sentry").
#
# The dashboard must run on whichever Pi physically owns the LoRa receiver's
# serial port. That is the always-on gateway, NOT the rpi5 dev/Jenkins box.
# This makes the gateway the single owner of /dev/ttyACM* so there are never
# "several open serial instances" fighting over the port.
#
# RUN THIS ON sentry (it can reach the rpi5 source box, but not vice-versa):
#
#     ssh pi@sentry
#     cd ~/lora_web
#     ./deploy_to_sentry.sh            # pull latest + restart
#     ./deploy_to_sentry.sh --no-run   # pull latest only, don't (re)start
#
# Override the source if the rpi5 IP/path differs:
#     SRC_HOST=192.168.1.54 SRC_DIR=/home/pi/work/stm32/tools/lora_web ./deploy_to_sentry.sh
#
set -euo pipefail

SRC_HOST="${SRC_HOST:-192.168.1.54}"            # rpi5 dev box (source of truth)
SRC_USER="${SRC_USER:-pi}"
SRC_DIR="${SRC_DIR:-/home/pi/work/stm32/tools/lora_web}"
DEST_DIR="${DEST_DIR:-$HOME/lora_web}"
HTTP_PORT="${HTTP_PORT:-8080}"
RUN=1
[[ "${1:-}" == "--no-run" ]] && RUN=0

echo "[deploy] source : ${SRC_USER}@${SRC_HOST}:${SRC_DIR}"
echo "[deploy] dest   : ${DEST_DIR}"
mkdir -p "${DEST_DIR}"
cd "${DEST_DIR}"

# Back up the current code (never the DB) before overwriting.
stamp="$(date +%Y%m%d_%H%M%S)"
for f in server.py index.html; do
    [[ -f "$f" ]] && cp -a "$f" "${f}.bak_${stamp}" && echo "[deploy] backed up ${f} -> ${f}.bak_${stamp}"
done

# Pull only the code files. beacons.db stays local so gateway history is kept.
for f in server.py index.html; do
    rsync -av "${SRC_USER}@${SRC_HOST}:${SRC_DIR}/${f}" "./${f}"
done

echo "[deploy] syntax check..."
python3 -m py_compile server.py
echo "[deploy] server.py OK"

if [[ "${RUN}" -eq 1 ]]; then
    # Preferred path: the server runs under systemd (lora_web.service). A
    # restart cleanly stops the old process (releasing the serial port) and
    # starts the new code as the single owner.
    if systemctl list-unit-files lora_web.service >/dev/null 2>&1 \
            && systemctl cat lora_web.service >/dev/null 2>&1; then
        echo "[deploy] restarting systemd service lora_web..."
        sudo systemctl restart lora_web
        sleep 3
        if systemctl is-active --quiet lora_web; then
            echo "[deploy] active. Open http://$(hostname):${HTTP_PORT}/"
            echo "[deploy] logs: journalctl -u lora_web -f"
            systemctl --no-pager --lines=6 status lora_web | sed 's/^/    /' || true
        else
            echo "[deploy] ERROR: service failed to start. Recent logs:" >&2
            journalctl -u lora_web --no-pager --lines=20 >&2 || true
            exit 1
        fi
    else
        # Fallback (service not installed): run detached with nohup.
        echo "[deploy] lora_web.service not installed; starting with nohup..."
        echo "[deploy] (install the service with: sudo install -m 644 ${DEST_DIR}/lora_web.service /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable --now lora_web)"
        pkill -f "lora_web/server.py" 2>/dev/null || true
        pkill -f "${DEST_DIR}/server.py" 2>/dev/null || true
        sleep 1.5
        echo "[deploy] starting on :${HTTP_PORT} (auto-detect serial port)..."
        # No --port: auto-detect survives ttyACM0<->ttyACM1 re-enumeration.
        setsid python3 server.py --http-port "${HTTP_PORT}" \
            >"${DEST_DIR}/lora_web.log" 2>&1 < /dev/null &
        sleep 3
        if pgrep -f "${DEST_DIR}/server.py" >/dev/null || pgrep -f "lora_web/server.py" >/dev/null; then
            echo "[deploy] running. Open http://$(hostname):${HTTP_PORT}/"
            echo "[deploy] log: ${DEST_DIR}/lora_web.log"
            tail -n 6 "${DEST_DIR}/lora_web.log" || true
        else
            echo "[deploy] ERROR: server did not start. Last log lines:" >&2
            tail -n 20 "${DEST_DIR}/lora_web.log" >&2 || true
            exit 1
        fi
    fi
else
    echo "[deploy] --no-run: not (re)starting. Restart manually with:"
    echo "         sudo systemctl restart lora_web    # if service installed"
    echo "         cd ${DEST_DIR} && python3 server.py --http-port ${HTTP_PORT}"
fi
