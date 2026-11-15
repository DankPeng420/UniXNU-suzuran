# Runs on the phone in TWRP (/tmp): model check + unmount /data. Writes nothing.
echo "device: $(getprop ro.product.device) / $(getprop ro.product.model)"
case "$(getprop ro.product.device)$(getprop ro.product.model)" in
  *suzuran*|*E5823*) ;;
  *) echo "REFUSE: not a Z5 Compact"; exit 1 ;;
esac
umount /sdcard 2>/dev/null; umount /data/media 2>/dev/null; umount /data 2>/dev/null
if grep -qE "mmcblk0p42| /data " /proc/mounts; then echo "REFUSE: /data still mounted:"; grep -E "mmcblk0p42| /data " /proc/mounts; exit 1; fi
echo PREP_OK
