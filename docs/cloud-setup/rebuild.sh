#!/bin/bash
# Fast incremental loop: rebuild only the openmw target (ccache + mold make this seconds-minutes), then optionally run.
# Usage: rebuild.sh [run]     Edit code in /home/user/openmw first.
cd /home/user/openmw-build && ninja openmw 2>&1 | tail -15 && [ "$1" = run ] && exec /home/user/openmw/docs/cloud-setup/run.sh
