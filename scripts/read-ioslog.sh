#!/bin/bash
# Read iOS's own unified log (and crash reports) from a copy of the phone's "ios" partition, on a Mac.
#   1. Phone in Android: tools/phone/pull-ios-partition.sh p55.img.gz   (~20 min, read-only)
#   2. Here: read-ioslog.sh p55.img.gz [workdir]   -> <workdir>/boot-<bootuuid8>.txt, one line per entry
# Needs Mandiant's parser (github.com/mandiant/macos-UnifiedLogs, cargo build --release in
# examples/unifiedlog_iterator); point UNIFIEDLOG_ITERATOR at the binary. Apple's `log show` refuses the
# archive (1970 clock, no live metadata). The parser's log-archive mode has names and messages but wrong
# times; single-file mode has raw continuous ticks (625/12 ns each on the Lumia). merge-ioslog.py pairs
# the two per (pid, thread).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=${2:-$PWD/p55}
P=${UNIFIEDLOG_ITERATOR:?set UNIFIEDLOG_ITERATOR to the unifiedlog_iterator binary}
mkdir -p "$W" && cd "$W"
gunzip -c "$IMAGE" > p55.dmg && rm -f p55.shadow
D=$(hdiutil attach -nomount -shadow p55.shadow -imagekey diskimage-class=CRawDiskImage p55.dmg | awk '{print $1}')
mkdir -p mnt && /sbin/mount_hfs -o noowners "$D" mnt       # journal replays into p55.shadow only
DB=mnt/private/var/db; A=yuzu.logarchive
rm -rf $A sf && mkdir -p $A/Extra sf
cp -R $DB/diagnostics/{Persist,Special,Signpost,HighVolume,timesync} $A/ 2>/dev/null || true
cp -R $DB/uuidtext/* $A/
$P -m log-archive -i $A -o ioslog.jsonl 2>parse.err
for f in $(find $A/Persist $A/Special $A/Signpost $A/HighVolume -name '*.tracev3' -size +0); do
	n=$(echo "$f" | sed "s#$A/##; s#/#_#"); $P -m single-file -i "$f" -o "sf/$n.jsonl" 2>/dev/null
done
python3 "$HERE/merge-ioslog.py"
echo "crash reports: $W/mnt/private/var/mobile/Library/Logs/CrashReporter"
