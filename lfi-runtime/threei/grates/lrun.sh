#!/bin/bash
# Runs a grate chain inside a lindfs-style root (like the example grates' test
# harness): the sandbox's "/" is $LFS, with /dev from the host.
#   lrun.sh /grates/imfs-grate /grates/fs-view-grate /tests/x/test_cage ...
# The first argument is a path inside $LFS (the runtime loads it from the host);
# the others are exec'd by the grates inside the sandbox. Env: RUN, LFS, WD.
exec "$RUN" -r --dir=/="$LFS" --dir=/dev=/dev --wd="${WD:-/}" ${LRUN_OPTS:-} "$LFS$1" "${@:2}"
