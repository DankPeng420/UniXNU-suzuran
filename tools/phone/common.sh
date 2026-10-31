# Shared settings for the phone helpers. They run on the build host with the phone booted to Android
# (LineageOS, adb root allowed), and touch only the partitions named here.
#   ADB          adb binary (default: adb)
#   ESP_DEV      the EFIESP FAT partition (Lumia 950 XL: mmcblk0p35)
#   IOS_DEV      the GPT partition named "ios" that holds the iOS root (created by you; see INSTALL.md)
#   PAYLOAD_DIR  where DarwinBoot reads the kernel from (/cache is ext4, read through EfiFs's ext2 driver)
ADB=${ADB:-adb}
ESP_DEV=${ESP_DEV:-/dev/block/mmcblk0p35}
IOS_DEV=${IOS_DEV:-/dev/block/by-name/ios}
PAYLOAD_DIR=${PAYLOAD_DIR:-/cache/EFI/UniXNU/Payloads}

adb_root() {
	"$ADB" root >/dev/null
	sleep 5
	"$ADB" wait-for-device
}

# Run a shell snippet on the phone with the EFIESP mounted at /data/local/tmp/esp ($1: ro or rw).
with_esp() {
	mode=$1
	shift
	"$ADB" shell "set -e; E=/data/local/tmp/esp; mkdir -p \$E; mount -t vfat -o $mode $ESP_DEV \$E; $*; sync; umount \$E"
}
