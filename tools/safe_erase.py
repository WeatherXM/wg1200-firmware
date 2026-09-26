#!/usr/bin/env python3
#
# Wrap PlatformIO's "Erase Flash" target so it snapshots the esp_secure_cert
# partition (0xd000) BEFORE wiping the chip.
#
# The platform's `erase` target runs "$ERASECMD" (esptool erase_flash) directly,
# bypassing the custom upload_command -- so a plain `pio run -t erase` / the IDE
# "Erase Flash" button would otherwise destroy the irreplaceable factory
# credentials with no backup. "$ERASECMD" is substituted at run time, so a
# post: script can safely redirect it here.
#
# Must be a post: script: the platform's builder sets ERASECMD in its main
# script, so a pre: override would be clobbered.
#

Import("env")
import os

wrapper = os.path.normpath(
    os.path.join(env.subst("$PROJECT_DIR"), "tools", "upload_signed.py")
)
chip = env.BoardConfig().get("build.mcu", "esp32s3")

env.Replace(
    ERASECMD='"$PYTHONEXE" "%s" --erase "$UPLOAD_PORT" $UPLOAD_SPEED %s'
    % (wrapper, chip)
)
