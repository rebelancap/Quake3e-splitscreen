#!/bin/sh
# usage: scripts/tests/r13b-long.sh
# R13b item 1: the coordinator runs from a ~200-character fs_homepath under work/ (created here,
# deleted afterwards; the logs/screens are kept as work/r13b-long*) with a ~7400-character
# cl_splitChildArgs, so the first child line is over MAX_CMDLINE_CHARS; r13b-long.cfg then tries
# too many "+" commands and finally a normal child (which must boot with net_port 27981).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
D="r13b-longhome-"; while [ ${#D} -lt 155 ]; do D="${D}abcdefghij"; done
H="$REPO/work/${D:0:155}"
echo "homepath: $(cygpath -w "$H") ($(cygpath -w "$H" | wc -c) chars)" > "$REPO/work/r13b-long-home.txt"
PAD=$(printf '%*s' 7400 '' | tr ' ' x)
R13_PREFIX=r13b R13_HOME="$H" R13_CHILDARGS="+set developer 1 +set r13bpad $PAD" \
  sh "$REPO/scripts/tests/r13-run.sh" r13b-long.cfg long q3dm7
rm -rf "$H"
