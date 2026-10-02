#!/usr/bin/env bash
# Re-enables SAP GUI scripting on the Bigfox/A4H TEST system after a restart reset the profile parameter
# (symptom: fairyfly says "SAP GUI Scripting is disabled on the server ... sapgui/user_scripting = TRUE").
# Uses erpl-adt (ADT on port 50000), creates the console class ZCL_FF_ENABLE_SCRIPTING in $TMP once, then runs it:
# it sets sapgui/user_scripting = TRUE with the same dynamic switch as RZ11 (not persistent across restarts).
# The password is read from trial.env at run time and never printed. ONLY for the ephemeral test system.
# After it ran, close the old SAP GUI connection and log in again: the flag is read at logon
#   fairyfly session launch Bigfox --login
set -euo pipefail
cd "$(dirname "$0")/../.."
export MSYS_NO_PATHCONV=1
export SAP_PASSWORD="$(sed -n 's/.*Password: \([^ ]*\).*/\1/p' trial.env | head -1)"
adt() { timeout "${ADT_TIMEOUT:-120}" uvx erpl-adt --host bigfox --port 50000 --user DEVELOPER --client 001 "$@"; }
if ! adt source read ZCL_FF_ENABLE_SCRIPTING --type CLAS >/dev/null 2>&1; then
  adt object create --type CLAS/OC --name ZCL_FF_ENABLE_SCRIPTING --package '$TMP' --description "fairyfly: enable SAP GUI scripting (dynamic)"
fi
adt source write ZCL_FF_ENABLE_SCRIPTING --type CLAS --file tests/integration/abap/zcl_ff_enable_scripting.abap --activate
adt object run ZCL_FF_ENABLE_SCRIPTING
