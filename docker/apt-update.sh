#!/bin/sh
# `apt-get update` that survives Ubuntu archiving the base release (25.04 plucky is past EOL).
# Once Canonical moves a release to old-releases.ubuntu.com, the stock archive URLs stop serving it.
# This repoints apt there, but only when an Ubuntu archive reports the release gone, so other
# failures (outages, a broken third-party repo) still surface as before.
# It runs inside every apt step rather than once per stage: a one-off probe layer would stay cached
# from before the archival and never rewrite.
log=$(mktemp)
apt-get update -y "$@" >"$log" 2>&1
rc=$?
cat "$log"
if [ "$rc" -ne 0 ] && grep -qE '\.ubuntu\.com/ubuntu(-ports)? .*ha(s|ve) a Release file' "$log"; then
  rm -f "$log"
  # Archived ports (arm) releases live under old-releases' /ubuntu too, not /ubuntu-ports.
  sed -i -E 's#https?://(([a-z]{2}\.)?archive|security|ports)\.ubuntu\.com/ubuntu(-ports)?#http://old-releases.ubuntu.com/ubuntu#' \
    /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null
  # A mirror the sed doesn't know (e.g. a cloud-regional one) is left as is; fail as apt did.
  if grep -qs 'old-releases\.ubuntu\.com' /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; then
    echo "apt-update: release archived, switched apt sources to old-releases.ubuntu.com"
    exec apt-get update -y "$@"
  fi
  exit "$rc"
fi
rm -f "$log"
exit "$rc"
