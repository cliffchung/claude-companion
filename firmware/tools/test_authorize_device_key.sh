#!/bin/sh
# Tests for authorize_device_key.sh. Always uses a throwaway HOME, never ~/.ssh.
SCRIPT="$(cd "$(dirname "$0")" && pwd)/authorize_device_key.sh"
BASE="${TEST_TMP:-/tmp}/authkey-test.$$"
fail=0
pass() { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

B64="AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SsG6Yk0ahVkhEt2Ze1pTrcnXbpAZ2xdd"
KEY="ssh-ed25519 $B64 tab5@device"
mode() { stat -f %Lp "$1" 2>/dev/null || stat -c %a "$1"; }
newhome() { H="$BASE/$1"; mkdir -p "$H"; }

newhome t1
HOME="$H" sh "$SCRIPT" "$KEY" >/dev/null 2>&1; rc=$?
[ $rc -eq 0 ] && pass "accepts valid key" || bad "accepts valid key (rc=$rc)"
[ "$(mode "$H/.ssh")" = 700 ] && pass "~/.ssh is 700" || bad "~/.ssh mode"
[ "$(mode "$H/.ssh/authorized_keys")" = 600 ] && pass "file is 600" || bad "file mode"
want="from=\"100.64.0.0/10\" ssh-ed25519 $B64 claude-companion"
[ "$(cat "$H/.ssh/authorized_keys")" = "$want" ] && pass "line is correct" || bad "line is correct"

before=$(cat "$H/.ssh/authorized_keys")
HOME="$H" sh "$SCRIPT" "ssh-ed25519 $B64 other-comment" >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] && pass "duplicate refused" || bad "duplicate refused"
[ "$(cat "$H/.ssh/authorized_keys")" = "$before" ] && pass "file unchanged after duplicate" || bad "file changed"

# existing file without trailing newline, different key appended on its own line
newhome t2; mkdir -p "$H/.ssh"; printf 'ssh-rsa AAAAexisting me' > "$H/.ssh/authorized_keys"; chmod 600 "$H/.ssh/authorized_keys"
HOME="$H" sh "$SCRIPT" "$KEY" >/dev/null 2>&1
[ "$(wc -l < "$H/.ssh/authorized_keys" | tr -d ' ')" = 2 ] && [ "$(tail -n1 "$H/.ssh/authorized_keys")" = "$want" ] && pass "appends after unterminated line" || bad "append after unterminated line"

reject() {
    name="$1"; arg="$2"; newhome "r$$_$name"
    HOME="$H" sh "$SCRIPT" "$arg" >/dev/null 2>&1; rc=$?
    if [ $rc -ne 0 ] && [ ! -s "$H/.ssh/authorized_keys" ]; then pass "rejects $name"; else bad "rejects $name (rc=$rc)"; fi
}
reject garbage "hello world"
reject empty ""
reject badtype "ssh-dss $B64"
reject nonbase64 "ssh-ed25519 not*base64!"
reject options "command=\"rm -rf /\" ssh-ed25519 $B64"
reject from_options "from=\"1.2.3.4\",no-pty ssh-ed25519 $B64 x"
reject newline "ssh-ed25519 $B64 c
command=\"evil\" ssh-ed25519 $B64"
reject ecdsa_bad "ecdsa-sha2-nistp256"

newhome e1
HOME="$H" sh "$SCRIPT" "ecdsa-sha2-nistp256 AAAAE2VjZHNh c" >/dev/null 2>&1 && pass "accepts ecdsa" || bad "accepts ecdsa"
newhome e2
HOME="$H" sh "$SCRIPT" "ssh-ed25519 $B64" >/dev/null 2>&1 && pass "accepts key without comment" || bad "no comment"

rm -rf "$BASE"
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $fail
