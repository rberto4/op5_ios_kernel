#!/bin/bash
# Background crash loops of the 0136-0140 boots (logs/stage2-0136-perf-live.txt), switched off on the running phone over SSH.
#
#   tools/op5-crashloop-root.sh apply    the changes below (backups in /var/root/crashloop-backup and backup/crashloop-*/)
#   tools/op5-crashloop-root.sh undo     everything back as it was
#   tools/op5-crashloop-root.sh status
#
# What loops (one 0136 boot, ~25 min): SiriTTSSynthesizerAU, MacinTalkAUSP, KonaSynthesizer 50 exec + 50 SIGABRT each
# (an NSException in PlugInKit while the extension starts: CFDictionaryCreate with nil), AegirPoster 38 exec, 32 SIGABRT +
# 6 "extension check-in timeout" (stuck in CA::Context::connect_remote: the Astronomy poster renders with Metal),
# nanotimekitcompaniond 175 exec (the Apple Watch face daemon, relaunched on every LaunchServices.ApplicationsChanged).
# Each crash also wakes ReportCrash, launchd and coreduetd (28832 AppleKeyStore calls from coreduetd alone).
#
# The changes: the four extensions renamed in place to NAME.appex.sweet-off (PlugInKit only lists *.appex; nothing is
# deleted), nanotimekitcompaniond added to launchd's disabled.plist. Reboot afterwards.
# Not touched: assetsd (15 SIGABRT, the Photos library daemon: Photos needs it), driverkitd (4 SIGKILL).
set -euo pipefail
cd "$(dirname "$0")/.."
SSH=(ssh -F ../dumps/ssh_config op5)
SCP=(scp -O -F ../dumps/ssh_config)
DIS=/private/var/db/com.apple.xpc.launchd/disabled.plist
JOBS=(com.apple.nanotimekitcompaniond com.apple.voicemail.vmd)
APPEX=(
	/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/PlugIns/KonaSynthesizer.appex
	/System/Library/PrivateFrameworks/MacinTalk.framework/PlugIns/MacinTalkAUSP.appex
	/System/Library/PrivateFrameworks/SiriTTSService.framework/PlugIns/SiriTTSSynthesizerAU.appex
	/System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex
)
local_bk=backup/crashloop-$(date +%Y%m%d-%H%M%S)

# disabled.plist: pulled, edited here with plistlib (the phone has no plutil), pushed back. $1 = true (apply) / remove (undo)
jobs_edit() {
	mkdir -p "$local_bk"
	"${SCP[@]}" "op5:$DIS" "$local_bk/disabled.plist"
	python3 -I - "$local_bk/disabled.plist" "$local_bk/disabled.new.plist" "$1" "${JOBS[@]}" <<'EOF'
import plistlib, sys
src, dst, mode, jobs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
d = plistlib.load(open(src, 'rb'))
for j in jobs:
    if mode == 'true':
        d[j] = True
    else:
        d.pop(j, None)
plistlib.dump(d, open(dst, 'wb'), fmt=plistlib.FMT_BINARY)
print('disabled.plist:', ', '.join(f'{k}={v}' for k, v in sorted(d.items())))
EOF
	"${SCP[@]}" "$local_bk/disabled.new.plist" "op5:$DIS.new"
	"${SSH[@]}" "mkdir -p /var/root/crashloop-backup && cp -p $DIS /var/root/crashloop-backup/disabled.plist.\$(date +%s) &&
		chown root:wheel $DIS.new && chmod 644 $DIS.new && mv $DIS.new $DIS"
	echo "local copies in $local_bk"
}

case ${1:-} in
apply)
	for a in "${APPEX[@]}"; do
		"${SSH[@]}" "if [ -d '$a' ]; then mv '$a' '$a.sweet-off' && echo 'off: $a'; else echo 'not there (already off?): $a'; fi"
	done
	jobs_edit true
	echo "now reboot the phone"
	;;
undo)
	for a in "${APPEX[@]}"; do
		"${SSH[@]}" "if [ -d '$a.sweet-off' ] && [ ! -d '$a' ]; then mv '$a.sweet-off' '$a' && echo 'back: $a'; fi"
	done
	jobs_edit remove
	echo "now reboot the phone"
	;;
status)
	for a in "${APPEX[@]}"; do
		"${SSH[@]}" "ls -d '$a' '$a.sweet-off' 2>/dev/null || true"
	done
	"${SSH[@]}" "ps -A -o pid,%cpu,comm 2>/dev/null | sort -k2 -rn | head -12 || ps -A | head -20"
	;;
*)
	sed -n '2,16p' "$0"; exit 2
	;;
esac
