#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
exec /bin/sh "$task_root/assets/integration/install.sh" --app "$task_root/dist/ikview.app" "$@"
