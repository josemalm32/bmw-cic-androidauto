#!/bin/ksh
# AAPlugin installer for the BMW CIC HIGH. Runs from the USB stick at CIC boot
# (the CIC executes /fs/usb0/copie_scr.sh, encoded, only during boot).
#
#  1. Copies existing AAPlugin logs from the CIC to the stick (AALOGS/) and
#     deletes them from the CIC.
#  2. Installs every file that differs from the stick payload (AAP/):
#       HDD   /mnt/hbdebug/aap : libcicaa.so cicaalive dspload hu.crt hu.key
#                                dvd_dm642_aa.bin (patched from the car's own DSP image)
#       flash /mnt/EFS_RO/Hmi/data/ro/Plugins/AAP : AAPlugin.so .xml .manifest
#  3. Removes the old development plugin (Plugins/CMD with CicMenuDemo.so),
#     backing it up to the stick (AALEGACY/).
#  4. Reboots the CIC only if something changed (so it never loops).
# Its own report goes to the stick only: AAINSTALL.TXT.
PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/bin:/mnt/EFS_RO/opt/bin:/mnt/EFS_RO/bin:/proc/boot
export PATH
umask 022
USB=/fs/usb0
PAY="$USB/AAP"
LOG="$USB/AAINSTALL.TXT"
DIR=/mnt/hbdebug/aap
PLUG=/mnt/EFS_RO/Hmi/data/ro/Plugins
TARGET="$PLUG/AAP"
LEGACY="$PLUG/CMD"
V=/tmp/aap_dspload
[ -d "$PAY" ] || exit 0
echo "AAPlugin installer @VERSION@" > "$LOG" || exit 80
exec >> "$LOG" 2>&1
flush() { /bin/sync 2>/dev/null; sync 2>/dev/null; }
date 2>/dev/null

# ---- 1. logs CIC -> stick ----
if [ -d "$DIR/logs" ]; then
 mkdir -p "$USB/AALOGS"
 for F in "$DIR"/logs/*; do
  [ -f "$F" ] || continue
  B=${F##*/}
  cp "$F" "$USB/AALOGS/$B" && rm -f "$F" && echo "LOG_COPIED $B"
 done
fi

# ---- helper tool (byte compare, DSP patch, reboot) from the stick ----
cp "$PAY/dspload" "$V" && chmod 755 "$V" || { echo NO_HELPER; flush; exit 81; }
"$V" verify "$V" "$PAY/dspload" || { echo HELPER_BAD; flush; exit 81; }
CHANGED=0
# inst SRC DST MODE: copy only if different, verify, atomic rename
inst() {
 if "$V" verify "$2" "$1"; then echo "SAME $2"; return 0; fi
 cp "$1" "$2.new" || { echo "CP_FAIL $2"; return 1; }
 "$V" verify "$2.new" "$1" || { echo "VERIFY_FAIL $2"; rm -f "$2.new"; return 1; }
 chmod "$3" "$2.new"
 mv -f "$2.new" "$2" || { echo "MV_FAIL $2"; return 1; }
 "$V" verify "$2" "$1" || { echo "FINAL_FAIL $2"; return 1; }
 echo "INSTALLED $2"; CHANGED=1
}

# ---- 2a. HDD files ----
mkdir -p "$DIR" || { echo NO_HDD_DIR; flush; exit 82; }
inst "$PAY/libcicaa.so" "$DIR/libcicaa.so" 644 || { flush; exit 82; }
inst "$PAY/cicaalive" "$DIR/cicaalive" 755 || { flush; exit 82; }
inst "$PAY/dspload" "$DIR/dspload" 755 || { flush; exit 82; }
if [ -f "$USB/AAHU.CRT" ] && [ -f "$USB/AAHU.KEY" ]; then
 inst "$USB/AAHU.CRT" "$DIR/hu.crt" 600 || { flush; exit 82; }
 inst "$USB/AAHU.KEY" "$DIR/hu.key" 600 || { flush; exit 82; }
elif [ ! -f "$DIR/hu.crt" ]; then
 echo "WARNING no AAHU.CRT/AAHU.KEY on the stick and none installed: Android Auto will not connect"
fi
# DSP image: always rebuilt from the car's own copy, installed only if different
"$V" patch /tmp/aap_dsp.bin || { echo DSP_PATCH_FAILED_UNKNOWN_DSP_IMAGE; flush; exit 83; }
inst /tmp/aap_dsp.bin "$DIR/dvd_dm642_aa.bin" 644 || { flush; exit 83; }
rm -f /tmp/aap_dsp.bin

# ---- 2b. plugin on flash ----
PCHG=0
for S in so xml manifest; do "$V" verify "$TARGET/AAPlugin.$S" "$PAY/AAPlugin.$S" || PCHG=1; done
LCHG=0
[ -f "$LEGACY/CicMenuDemo.so" ] && LCHG=1
if [ "$PCHG" = 1 ] || [ "$LCHG" = 1 ]; then
 if [ "$LCHG" = 1 ]; then
  mkdir -p "$USB/AALEGACY"
  cp "$LEGACY"/* "$USB/AALEGACY/" || { echo LEGACY_BACKUP_FAILED_NO_CHANGE; flush; exit 84; }
  echo "LEGACY_BACKED_UP"
 fi
 flush
 mount -uw /mnt/EFS_RO || mount -u -w /dev/fs0p3 /mnt/EFS_RO || { echo REMOUNT_RW_FAIL_NO_CHANGE; flush; exit 85; }
 OK=1
 if [ "$PCHG" = 1 ]; then
  mkdir -p "$TARGET" || OK=0
  for S in so xml manifest; do
   [ "$OK" = 1 ] || break
   cp "$PAY/AAPlugin.$S" "$TARGET/.AAPlugin.$S.new" && "$V" verify "$TARGET/.AAPlugin.$S.new" "$PAY/AAPlugin.$S" || OK=0
  done
  if [ "$OK" = 1 ]; then
   for S in so xml manifest; do
    chmod 555 "$TARGET/.AAPlugin.$S.new"
    mv -f "$TARGET/.AAPlugin.$S.new" "$TARGET/AAPlugin.$S" && "$V" verify "$TARGET/AAPlugin.$S" "$PAY/AAPlugin.$S" || OK=0
   done
  fi
  rm -f "$TARGET"/.AAPlugin.*.new
 fi
 if [ "$OK" = 1 ] && [ "$LCHG" = 1 ]; then
  rm -f "$LEGACY"/* && rmdir "$LEGACY" && echo "LEGACY_REMOVED" || echo "LEGACY_REMOVE_FAILED"
 fi
 flush; mount -ur /mnt/EFS_RO || mount -u -r /dev/fs0p3 /mnt/EFS_RO; flush
 if [ "$OK" != 1 ]; then echo "PLUGIN_WRITE_FAILED (old files left in place)"; ls -l "$TARGET"; flush; exit 86; fi
 echo PLUGIN_INSTALLED; CHANGED=1
else
 echo PLUGIN_SAME
fi
ls -l "$TARGET" "$DIR"
flush
if [ "$CHANGED" = 1 ]; then
 echo "INSTALLED_REBOOTING"; flush
 "$DIR/dspload" sysreboot
else
 echo "UP_TO_DATE_NO_REBOOT"
fi
flush
exit 0
