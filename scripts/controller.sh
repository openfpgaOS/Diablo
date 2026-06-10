#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# Minimal version - less likely to lock your keyboard completely

echo "=== Analogue Pocket Dock Keyboard Controller (Minimal) ==="

# Auto-detect keyboard
KEYBOARD=$(python3 -c '
import evdev
for p in evdev.list_devices():
    d = evdev.InputDevice(p)
    if any(k in d.name.lower() for k in ["keyboard", "kbd", "key"]):
        print(p)
        break
' 2>/dev/null)

if [ -z "$KEYBOARD" ]; then
    echo "Keyboard not detected. List devices:"
    echo "python3 -c 'import evdev; [print(d.path, \"-\", d.name) for d in [evdev.InputDevice(p) for p in evdev.list_devices()]]'"
    exit 1
fi

echo "Using: $KEYBOARD"
echo "Mappings: WASD/Arrows → D-Pad | Z X C V → A B X Y | Q E → LB RB | Space → Start | Shift → Back"
echo "Run script FIRST, then plug USB cable into Dock."

sudo xboxdrv \
  --evdev "$KEYBOARD" \
  --evdev-keymap KEY_W=du,KEY_S=dd,KEY_A=dl,KEY_D=dr,KEY_UP=du,KEY_DOWN=dd,KEY_LEFT=dl,KEY_RIGHT=dr,KEY_Z=a,KEY_X=b,KEY_C=x,KEY_V=y,KEY_Q=lb,KEY_E=rb,KEY_SPACE=start,KEY_LEFTSHIFT=back \
  --mimic-xpad \
  --silent \
  --detach-kernel-driver