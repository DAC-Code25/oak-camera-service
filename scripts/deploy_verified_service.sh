#!/usr/bin/env bash
set -euo pipefail
if [[ ${EUID} -ne 0 || $# -ne 2 ]]; then
  echo "Usage: sudo $0 VERIFIED_BINARY EXPECTED_SHA256" >&2
  exit 2
fi
project_root=/home/nvidia/tenco-project
script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
[[ "$script_root" == "$project_root/oak-camera-service/scripts" ]] || {
  echo "Deploy only from the managed project workspace" >&2; exit 2;
}
# Shared project-local transaction checks the actual camera child process.
# Readiness-only mode creates neither photos nor recordings.
exec /usr/bin/python3 "$script_root/deploy_media_settings.py" "$1" "$2" \
  --readiness-only \
  --evidence "$project_root/tmp/camera-deploy-$(date +%Y%m%d-%H%M%S)-$$.json"
