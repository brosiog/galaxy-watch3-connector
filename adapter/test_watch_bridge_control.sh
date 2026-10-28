#!/bin/sh
# Invalid input must fail before a command is sent. The private directory has no adapter.
set -eu
control=$1
dir=$(mktemp -d /tmp/pbl-control-review.XXXXXX)
trap 'rm -rf "$dir"' EXIT
export PBL_RUNTIME_DIR="$dir"

expect_status() {
  expected=$1
  shift
  result=0
  "$control" "$@" >"$dir/output" 2>&1 || result=$?
  if [ "$result" -ne "$expected" ]; then
    printf 'FAIL control input: expected exit %s, got %s\n' "$expected" "$result" >&2
    cat "$dir/output" >&2
    exit 1
  fi
}

expect_status 2 idle 0 garbage
expect_status 2 idle 0 -1
expect_status 2 idle 0 ' -1'
expect_status 2 idle 0 18446744073709551616
expect_status 2 idle 0 ''
expect_status 2 backlight ''
expect_status 2 hrm ''
# Valid commands get past parsing and fail to connect to the absent adapter.
expect_status 1 idle 0 0
expect_status 1 idle 0 1099511627776
expect_status 1 backlight 0
echo BRIDGE_CONTROL_INPUT_VALIDATION_OK
