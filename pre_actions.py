#
# Rename output to fw_[build type]_[version].bin
#

import warnings
warnings.filterwarnings("ignore")
import os
os.environ["PYTHONWARNINGS"] = "ignore"

Import("env")
import re
import subprocess

def get_git_commit():
    try:
        # Get the short commit hash (e.g., abc1234)
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], stderr=subprocess.DEVNULL).decode("utf-8").strip()
        if not commit:
            return ""
        # Check if the repository has any uncommitted changes (dirty)
        # git diff --quiet exits with 1 if there are changes, 0 if clean
        status = subprocess.run(["git", "diff", "--quiet"], stderr=subprocess.DEVNULL)
        dirty = "-dirty" if status.returncode != 0 else ""
        return f"g{commit}{dirty}"
    except Exception:
        return ""

def write_file_if_changed(filepath, content):
    if os.path.exists(filepath):
        with open(filepath, "r") as f:
            existing = f.read()
        if existing == content:
            return
    # Ensure directory exists
    dirpath = os.path.dirname(filepath)
    if dirpath and not os.path.exists(dirpath):
        os.makedirs(dirpath)
    with open(filepath, "w") as f:
        f.write(content)

# Environment variable override takes highest precedence (e.g. CI/CD or explicit build command)
env_fw_version = os.environ.get("FIRMWARE_VERSION", "").strip()
if env_fw_version:
    full_version = env_fw_version
else:
    # Find base version in app config
    with open("main/include/app_config.h", "r") as f_config:
        app_config = f_config.read()

    version_match = re.findall(r'#define APP_VERSION_BASE\s+"([^"]+)"', app_config)
    if version_match:
        base_version = version_match[0]
    else:
        # Fallback to APP_VERSION if APP_VERSION_BASE is not found
        version_match = re.findall(r'#define APP_VERSION\s+"([^"]+)"', app_config)
        base_version = version_match[0] if version_match else "0.0.0"

    git_suffix = get_git_commit()
    if git_suffix:
        full_version = f"{base_version}-{git_suffix}"
    else:
        full_version = base_version

# ESP-IDF limits esp_app_desc_t.version to 32 bytes (including null terminator).
if len(full_version) > 31:
    print(f"--- WARNING: Firmware version '{full_version}' ({len(full_version)} chars) exceeds ESP-IDF limit of 31 chars! Truncation may occur in esp_app_desc_t.version ---")

print(f"--- Firmware Build Version: {full_version} ---")

# Synchronize version.txt for ESP-IDF CMake (esp_app_desc_t.version)
project_dir = env.subst("$PROJECT_DIR")
version_txt_path = os.path.join(project_dir, "version.txt")
cmakelists_path = os.path.join(project_dir, "CMakeLists.txt")

version_changed = False
if not os.path.exists(version_txt_path):
    version_changed = True
else:
    try:
        with open(version_txt_path, "r") as f:
            if f.read().strip() != full_version:
                version_changed = True
    except Exception:
        version_changed = True

if version_changed:
    with open(version_txt_path, "w") as f:
        f.write(full_version + "\n")
    # Touch CMakeLists.txt to invalidate PlatformIO's CMake cache so it reconfigures
    # and recompiles esp_app_desc.c in esp_app_format component
    if os.path.exists(cmakelists_path):
        os.utime(cmakelists_path, None)
        print(f"--- Synchronized version.txt and refreshed CMakeLists.txt mtime for PROJECT_VER: {full_version} ---")

# Generate local build_version.h for main application
build_version_content = f'#pragma once\n#define APP_VERSION "{full_version}"\n'
write_file_if_changed("main/include/build_version.h", build_version_content)

# Generate local fw_version.h for components/esp32-wifi-manager
fw_version_content = f'#define APP_VERSION "{full_version}"\n'
write_file_if_changed("components/esp32-wifi-manager/src/fw_version.h", fw_version_content)

# Ensure open station key headers exist from templates
import shutil
ws1300_key_h = "main/ws/supported/ws_1300/include/ws_1300_keys.h"
ws1300_key_tpl = "main/ws/supported/ws_1300/include/ws_1300_keys.h.template"
if not os.path.exists(ws1300_key_h) and os.path.exists(ws1300_key_tpl):
    shutil.copyfile(ws1300_key_tpl, ws1300_key_h)
    print("--- [Open Keys] Initialized ws_1300_keys.h from template ---")

ws1001_key_h = "main/ws/supported/ws_1001/include/ws_1001_keys.h"
ws1001_key_tpl = "main/ws/supported/ws_1001/include/ws_1001_keys.h.template"
if not os.path.exists(ws1001_key_h) and os.path.exists(ws1001_key_tpl):
    shutil.copyfile(ws1001_key_tpl, ws1001_key_h)
    print("--- [Open Keys] Initialized ws_1001_keys.h from template ---")

def generate_compressed_dashboard():
    import gzip
    web_server_path = "main/ws/web_server.cpp"
    out_header = "main/ws/include/dashboard_html_gz.h"
    if not os.path.exists(web_server_path):
        return
    with open(web_server_path, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r'static const char g_dashboard_html\[\] = R\"html\((.*?)\)html\";', content, re.DOTALL)
    if not match:
        return
    html_text = match.group(1)
    gz_data = gzip.compress(html_text.encode("utf-8"), compresslevel=9, mtime=0)
    lines = [
        '#pragma once',
        '#include <stdint.h>',
        '#include <stddef.h>',
        '',
        '// Auto-generated compressed dashboard HTML',
        f'// Original size: {len(html_text)} bytes, Gzip size: {len(gz_data)} bytes ({len(gz_data)/len(html_text)*100:.1f}%)',
        'static const uint8_t g_dashboard_html_gz[] = {'
    ]
    for i in range(0, len(gz_data), 16):
        chunk = gz_data[i:i+16]
        lines.append('  ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    lines.append(f'static const size_t g_dashboard_html_gz_len = {len(gz_data)};')
    header_content = '\n'.join(lines) + '\n'
    write_file_if_changed(out_header, header_content)

generate_compressed_dashboard()

env.Replace(PROGNAME="fw_%s_%s" % (env.GetBuildType(), full_version))

#
# Dynamic Upload Flags: Hard Reset after upload by default, unless WXM_NO_RESET=1
#
no_reset = os.environ.get("WXM_NO_RESET", "0") == "1" or os.path.exists(".no_reset")
upload_flags = ["--before", "default_reset"]
if no_reset:
    upload_flags.extend(["--after", "no_reset"])
    print("--- Flash upload configuration: NO RESET after upload ---")
else:
    upload_flags.extend(["--after", "hard_reset"])
    print("--- Flash upload configuration: HARD RESET (restart) after upload (default) ---")

env.Replace(UPLOAD_FLAGS=upload_flags)

#
# Force device reboot by default after platformio upload, even in Secure Boot mode.
# By registering a post-action on the "upload" target, we can execute esptool.py
# with a fast, non-destructive flash_id command that does a hardware reset
# after the upload process has successfully completed.
#
def run_post_upload_reboot(source, target, env):
    import os
    import sys
    import subprocess

    no_reset = os.environ.get("WXM_NO_RESET", "0") == "1" or os.path.exists(".no_reset")
    if no_reset:
        print("--- post_upload_reboot: NO RESET requested, skipping device reboot ---")
        return

    print("--- post_upload_reboot: Forcing device reboot via esptool ---")
    
    # 1. Resolve upload port and speed
    upload_port = env.get("UPLOAD_PORT", "")
    upload_speed = env.get("UPLOAD_SPEED", "115200")
    
    # 2. Resolve esptool.py path
    esptool_path = "esptool.py"  # fallback
    try:
        platform = env.PioPlatform()
        esptool_dir = platform.get_package_dir("tool-esptoolpy")
        if esptool_dir and os.path.exists(os.path.join(esptool_dir, "esptool.py")):
            esptool_path = os.path.join(esptool_dir, "esptool.py")
    except Exception as e:
        print(f"--- post_upload_reboot: Could not resolve esptool path from platform: {e} ---")
        
    cmd = [
        sys.executable, esptool_path,
        "--chip", "esp32s3",
    ]
    if upload_port:
        cmd.extend(["--port", str(upload_port)])
    if upload_speed:
        cmd.extend(["--baud", str(upload_speed)])
        
    cmd.extend(["--after", "hard_reset", "flash_id"])
    
    print(f"--- post_upload_reboot: Running command: {' '.join(cmd)} ---")
    
    # 3. Execute command
    try:
        subprocess.run(cmd, check=True)
        print("--- post_upload_reboot: Hardware reset command executed successfully ---")
    except Exception as e:
        print(f"--- post_upload_reboot: Hardware reset failed: {e} ---")

env.AddPostAction("upload", run_post_upload_reboot)

#
# Custom Upload Command for WG1200 (Secure Boot / Triple Slot Flash with --no-stub)
#
def custom_upload_wg1200(source, target, env):
    import os
    import sys
    import subprocess

    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("$PROGNAME")
    project_dir = env.subst("$PROJECT_DIR")
    repo_dir = project_dir
    
    # 1. Resolve paths to the signed and unsigned binary
    signed_bin = os.path.join(build_dir, f"{progname}-signed.bin")
    unsigned_bin = os.path.join(build_dir, f"{progname}.bin")
    
    target_bin = signed_bin
    if not os.path.exists(target_bin):
        if os.path.exists(unsigned_bin):
            print(f"--- custom_upload_wg1200: Signed binary not found, checking for key file to sign in-place ---")
            key_file = os.path.join(repo_dir, "secrets", "wg1200_secure_boot_key.pem")
                
            if os.path.exists(key_file):
                print(f"--- custom_upload_wg1200: Signing in-place {unsigned_bin} using {key_file} ---")
                sign_cmd = [
                    sys.executable,
                    "-m",
                    "espsecure",
                    "sign-data",
                    "--version", "2",
                    "--keyfile", key_file,
                    "--output", signed_bin,
                    unsigned_bin
                ]
                res = subprocess.run(sign_cmd)
                if res.returncode == 0:
                    target_bin = signed_bin
                else:
                    print("--- custom_upload_wg1200: Binary signing failed ---")
                    sys.exit(1)
            else:
                target_bin = unsigned_bin
                print(f"--- custom_upload_wg1200: Using unsigned binary {unsigned_bin} (no key found) ---")
        else:
            print(f"--- custom_upload_wg1200: Error: binary not found! ---")
            sys.exit(1)

    print(f"--- custom_upload_wg1200: Target binary for flashing is {target_bin} ---")
    
    # 2. Resolve port and speed
    upload_port = env.get("UPLOAD_PORT", "")
    upload_speed = env.get("UPLOAD_SPEED", "115200")
    try:
        # For secure boot triple-slot flash using --no-stub, capping baud rate to 115200 ensures rock-solid stability and prevents "chip stopped responding" errors.
        if int(upload_speed) > 115200:
            print(f"--- custom_upload_wg1200: Capping baud rate from {upload_speed} to 115200 for stable --no-stub flash ---")
            upload_speed = "115200"
    except Exception:
        upload_speed = "115200"
    
    # 2.5 Resolve partition CSV and calculate otadata offset for subprocess
    otadata_offset = "0x13000"  # fallback default for 16MB
    try:
        project_config = env.GetProjectConfig()
        partitions_csv = project_config.get(f"env:{pio_env}", "board_build.partitions", "partitions_16MB_wxm.csv")
        if os.path.exists(partitions_csv):
            with open(partitions_csv, "r") as f:
                for line in f:
                    if line.strip().startswith("#") or not line.strip():
                        continue
                    parts = [p.strip() for p in line.split(",")]
                    if len(parts) > 3 and parts[0] == "otadata":
                        otadata_offset = parts[3]
                        print(f"--- custom_upload_wg1200: Detected otadata offset from {partitions_csv}: {otadata_offset} ---")
                        break
    except Exception as e:
        print(f"--- custom_upload_wg1200: Error detecting otadata offset: {e} ---")

    os.environ["WXM_OTADATA_OFFSET"] = otadata_offset

    # 3. Resolve upload script path
    upload_script = os.path.normpath(os.path.join(project_dir, "tools", "upload_signed.py"))
    
    # 4. Formulate command to run the upload script
    cmd = [
        sys.executable,
        upload_script,
        str(upload_port),
        str(upload_speed),
        target_bin
    ]
    
    print(f"--- custom_upload_wg1200: Invoking custom upload script: {' '.join(cmd)} ---")
    
    try:
        subprocess.run(cmd, check=True)
        print("--- custom_upload_wg1200: Upload completed successfully! ---")
        import sys
        sys.exit(0)
    except Exception as e:
        print(f"--- custom_upload_wg1200: Upload failed: {e} ---")
        import sys
        sys.exit(1)

pio_env = env.subst("$PIOENV")
if "wg1200" in pio_env:
    print(f"--- custom_upload_wg1200: Registering secure boot upload pre-action hook for {pio_env} ---")
    env.AddPreAction("upload", custom_upload_wg1200)

#
# Auto-copy build artifacts (.elf, .bin, and local -signed.bin) to the releases/ directory
# for easy Git tracking and version commits.
#
def copy_build_artifacts(source, target, env):
    import shutil
    import os
    
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("$PROGNAME")
    project_dir = env.subst("$PROJECT_DIR")
    
    releases_dir = os.path.join(project_dir, "releases")
    os.makedirs(releases_dir, exist_ok=True)
    
    files_to_copy = [
        os.path.join(build_dir, f"{progname}.elf"),
        os.path.join(build_dir, f"{progname}.bin"),
        os.path.join(build_dir, f"{progname}-signed.bin"),
    ]
    
    print("\n--- Copying build artifacts to releases/ folder ---")
    for f_path in files_to_copy:
        if os.path.exists(f_path):
            dest_path = os.path.join(releases_dir, os.path.basename(f_path))
            try:
                shutil.copy2(f_path, dest_path)
                print(f"--- [Copied] {os.path.basename(f_path)} -> releases/ ---")
            except Exception as e:
                print(f"--- [Copy Failed] {os.path.basename(f_path)}: {e} ---")

env.AddPostAction("buildprog", copy_build_artifacts)
