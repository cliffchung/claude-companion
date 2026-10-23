#!/bin/sh
# Authorize the Tab5's SSH public key on this Mac, for tailnet addresses only.
#   sh authorize_device_key.sh "ssh-ed25519 AAAA... [comment]"
# Appends   from="100.64.0.0/10" <type> <base64> claude-companion
# to $HOME/.ssh/authorized_keys. Refuses duplicates (matched on the base64 blob).
set -eu

die() { echo "authorize_device_key: $*" >&2; exit 1; }

[ $# -eq 1 ] || die "usage: $0 \"<ssh public key line>\""
line=$1
NL='
'
CR=$(printf '\r')
case $line in
    *"$NL"*|*"$CR"*) die "key must be a single line" ;;
esac

# Split into type, blob and optional comment without globbing.
set -f
# shellcheck disable=SC2086
set -- $line
set +f
[ $# -ge 2 ] || die "not an OpenSSH public key line"
type=$1
blob=$2

case $type in
    ssh-ed25519|ssh-rsa|ecdsa-sha2-*) ;;
    *) die "unsupported key type or options prefix: only 'ssh-ed25519|ssh-rsa|ecdsa-sha2-* <base64> [comment]'" ;;
esac
case $blob in
    *[!A-Za-z0-9+/=]*) die "key data is not base64" ;;
esac
[ -n "$blob" ] || die "empty key data"

ssh_dir=$HOME/.ssh
auth=$ssh_dir/authorized_keys

[ -d "$ssh_dir" ] || mkdir -p "$ssh_dir"
chmod 700 "$ssh_dir"
if [ ! -e "$auth" ]; then
    (umask 077; : > "$auth")
fi
chmod 600 "$auth"

# Duplicate check: any line with this exact blob as a whitespace-separated field.
if awk -v b="$blob" '{ for (i = 1; i <= NF; i++) if ($i == b) found = 1 } END { exit !found }' "$auth"; then
    die "that key is already in $auth; nothing changed"
fi

# Make sure we start on a fresh line if the file lacks a trailing newline.
if [ -s "$auth" ] && [ "$(tail -c1 "$auth" | wc -l | tr -d ' ')" = 0 ]; then
    printf '\n' >> "$auth"
fi
printf 'from="100.64.0.0/10" %s %s claude-companion\n' "$type" "$blob" >> "$auth"
echo "added $type key to $auth (tailnet 100.64.0.0/10 only)"
