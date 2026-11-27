# Sourced by each reproducer's ./run.  Usage of a reproducer:
#
#     ./run <host> [port] [secret]
#
# The certificates (CA, client certificate and key) come from $CERTS,
# default ../certs: ca.pem, client.crt, client.key, with the key's password
# in $KEY_PASSWORD (default "whatever", as in FreeRADIUS's raddb/certs).
#
# Each reproducer states what an RFC (or FreeRADIUS's own documentation)
# requires, runs eapol_test against the server, and checks that one fact
# in eapol_test's normal output.  It prints PASS or FAIL, and exits 0 or 1.
# Exit 2 means it could not run.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
NAME=$(basename "$HERE")

HOST=${1:?usage: ./run <host> [port] [secret]}
PORT=${2:-1812}
SECRET=${3:-testing123}
CERTS=${CERTS:-$TOP/certs}
KEY_PASSWORD=${KEY_PASSWORD:-whatever}
LOG=$HERE/eapol_test.log

# Build eapol_test with this reproducer's patch/*.patch (if any), once.
# Each set of patches gets its own build tree under .build/.
build_eapol_test() {
	local patches key dir
	patches=$(ls "$HERE"/patch/*.patch 2>/dev/null || true)
	key=$( (echo base; cat $patches /dev/null; cat "$TOP/eapol_test.config") | sha256sum | cut -c1-12)
	dir=$TOP/.build/$key
	if [ ! -x "$dir/wpa_supplicant/eapol_test" ]; then
		[ -f "$TOP/hostap/wpa_supplicant/eapol_test.c" ] || { echo "hostap submodule missing: git submodule update --init"; exit 2; }
		rm -rf "${dir:?}"
		mkdir -p "$TOP/.build"
		git -C "$TOP/hostap" worktree prune
		git -C "$TOP/hostap" worktree add -q --detach "$dir" HEAD
		for p in $patches; do git -C "$dir" apply "$p"; done
		cp "$TOP/eapol_test.config" "$dir/wpa_supplicant/.config"
		echo "building eapol_test ($([ -n "$patches" ] && echo "with $(echo $patches | xargs -n1 basename | tr '\n' ' ')" || echo unpatched)) ..." >&2
		make -s -C "$dir/wpa_supplicant" -j"$(nproc)" eapol_test \
			EXTRA_CFLAGS="-Wno-error=use-after-free -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=deprecated-declarations" \
			> "$dir/build.log" 2>&1 || { echo "eapol_test did not build: $dir/build.log"; exit 2; }
	fi
	EAPOL_TEST=$dir/wpa_supplicant/eapol_test
}

# The eapol_test configuration: $1 (a file here) with its placeholders
# filled in: @CERTS@, @KEY_PASSWORD@, @CLIENT_CERT@, and the identities
# @IDENTITY@ (EAP-TLS; default user@example.org), @ANON_IDENTITY@ (a
# tunnel's outer identity; default anonymous), @INNER_IDENTITY@ and
# @PASSWORD@ (inside a tunnel; default bob / bob).  Each can be set in
# the environment, for a server whose users differ.
config() {
	sed -e "s|@CERTS@|$CERTS|g" -e "s|@KEY_PASSWORD@|$KEY_PASSWORD|g" \
	    -e "s|@IDENTITY@|${IDENTITY:-user@example.org}|g" -e "s|@ANON_IDENTITY@|${ANON_IDENTITY:-anonymous}|g" \
	    -e "s|@INNER_IDENTITY@|${INNER_IDENTITY:-bob}|g" -e "s|@PASSWORD@|${PASSWORD:-bob}|g" \
	    -e "s|@CLIENT_CERT@|${CLIENT_CERT:-$CERTS/client.crt}|g" "$HERE/$1" > "$HERE/.$1"
	echo "$HERE/.$1"
}

# run_eapol_test <config> [eapol_test options...]: output goes to $LOG.
# Returns eapol_test's exit status (0 = authenticated).
run_eapol_test() {
	local conf=$1; shift
	[ -f "$CERTS/ca.pem" ] || { echo "no $CERTS/ca.pem: copy the server's test certificates there, or set CERTS"; exit 2; }
	set +e
	"$EAPOL_TEST" -c "$(config "$conf")" -a "$HOST" -p "$PORT" -s "$SECRET" -t 30 "$@" > "$LOG" 2>&1
	local rc=$?
	set -e
	return $rc
}

# Facts from eapol_test's output.
largest_request() {	# the largest EAP-Request from the server
	sed -n 's/^decapsulated EAP packet (code=1 id=[0-9]* len=\([0-9]*\)).*/\1/p' "${1:-$LOG}" | sort -n | tail -1
}
requests() {		# how many EAP-Requests the server sent (its rounds)
	grep -c '^decapsulated EAP packet (code=1 ' "${1:-$LOG}" || true
}
got_eap_failure() {	# did an EAP-Failure arrive from the server?
	grep -q '^decapsulated EAP packet (code=4 ' "${1:-$LOG}"
}
got_access_accept() {	# eapol_test authenticated
	grep -q '^SUCCESS$' "${1:-$LOG}"
}
got_access_reject() {
	grep -q 'RADIUS message: code=3 (Access-Reject)' "${1:-$LOG}"
}
challenges() {		# how many Access-Challenges the server sent (its rounds)
	grep -c 'RADIUS message: code=11 (Access-Challenge)' "${1:-$LOG}" || true
}

pass() { echo "PASS  $NAME: $*"; exit 0; }
fail() { echo "FAIL  $NAME: $*"; echo "      eapol_test output: $LOG"; exit 1; }

# The server's first EAP packet after the reproducer's first ACK, e.g.
# "code=1 id=188 len=6" (an empty Request) or "code=4 id=7 len=4".
answer_to_ack() {
	awk '/^REPRODUCER:/ { f = 1; next }
	     f && /^decapsulated EAP packet/ { sub(/^decapsulated EAP packet \(/, ""); sub(/\).*/, ""); print; exit }' "${1:-$LOG}"
}
