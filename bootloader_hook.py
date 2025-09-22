import os
import shutil
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
platform = env.PioPlatform()

# --- Configuration ---
FRAMEWORK_DIR = platform.get_package_dir("framework-espidf")

ORIGINAL_BOOTLOADER_SRC = os.path.join(
    FRAMEWORK_DIR, "components", "bootloader", "subproject", "main", "bootloader_start.c"
)


BACKUP_BOOTLOADER_SRC = ORIGINAL_BOOTLOADER_SRC + ".bak"
CUSTOM_BOOTLOADER_SRC = os.path.join(env.get("PROJECT_DIR"), "bootloader", "bootloader_start.c")

def backup_and_replace():
    """
    Backs up the original bootloader file and replaces it with the custom one.
    This function is called immediately when the script is loaded by PlatformIO.
    """
    if not os.path.exists(ORIGINAL_BOOTLOADER_SRC):
        print(f"ERROR: Original bootloader source not found at: {ORIGINAL_BOOTLOADER_SRC}")
        return

    # If a backup doesn't already exist, create one.
    if not os.path.exists(BACKUP_BOOTLOADER_SRC):
        print(f"Backing up original file to: {BACKUP_BOOTLOADER_SRC}")
        shutil.copy(ORIGINAL_BOOTLOADER_SRC, BACKUP_BOOTLOADER_SRC)
    else:
        print("Backup file already exists. Skipping backup.")

    # Copy our custom file over the original one.
    print(f"Replacing original bootloader with custom file: {CUSTOM_BOOTLOADER_SRC}")
    shutil.copy(CUSTOM_BOOTLOADER_SRC, ORIGINAL_BOOTLOADER_SRC)
    print("Replacement complete. Proceeding with build.")

def restore_original(source, target, env):
    """
    This function runs after the build is complete (on success or failure).
    It restores the original bootloader file from the backup.
    """
    if os.path.exists(BACKUP_BOOTLOADER_SRC):
        print(f"Restoring original bootloader file from: {BACKUP_BOOTLOADER_SRC}")
        shutil.copy(BACKUP_BOOTLOADER_SRC, ORIGINAL_BOOTLOADER_SRC)
        os.remove(BACKUP_BOOTLOADER_SRC)
        print("Restore complete. Backup file removed.")
    else:
        # This might happen if the pre-action failed
        print("No backup file found to restore.")

# --- SCRIPT EXECUTION ---

# 1. Run the replacement immediately when PlatformIO loads this script.
backup_and_replace()

# 2. Register the restore function to run *after* the build/upload process is finished.
env.AddPostAction("buildprog", restore_original)
env.AddPostAction("upload", restore_original)

# 3. Also register a pre-action for 'clean' to restore the original file
#    just in case a previous build failed and didn't clean up properly.
env.AddPreAction("clean", restore_original)