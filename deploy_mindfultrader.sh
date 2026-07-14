#!/bin/bash

# --- Configuration ---
# 1. Main DLL Configuration
# The source file from your build output, confirmed to be in the 'bin' directory.
SOURCE_FILE="${HOME}/devel/VSCode/MindfulTrader/build-windows/bin/MindfulTrader.dll"

# The destination path in Sierra Chart's data directory
DEST_FILE="/mnt/c/SierraChart2/Data/MindfulTrader.dll"
DEST_DIR=$(dirname "$DEST_FILE")

echo "--- Sierra Chart DLL Deployment Script ---"

# 1. Check if the required source file exists
if [ ! -f "$SOURCE_FILE" ]; then
    echo "Error: Main DLL source file not found."
    echo "Expected path: $SOURCE_FILE"
    echo "Please check your build output and try again."
    exit 1
fi

# 2. Check if the destination directory exists
if [ ! -d "$DEST_DIR" ]; then
    echo "Error: Destination directory not found."
    echo "Expected directory: $DEST_DIR"
    echo "Ensure Sierra Chart is installed and the path is correct."
    exit 1
fi

# 3. Backup the existing DLL (if it exists)
if [ -f "$DEST_FILE" ]; then
    # Create a unique, timestamped backup name
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    BACKUP_FILE="${DEST_DIR}/MindfulTrader.dll.bak_${TIMESTAMP}"

    echo "Existing DLL found. Creating timestamped backup..."
    if cp "$DEST_FILE" "$BACKUP_FILE"; then
        echo "   -> Backup successful: $BACKUP_FILE"
    else
        echo "Error: Failed to create backup. Check permissions."
        exit 1
    fi
else
    echo "No existing DLL found at destination. Skipping backup."
fi

# 4. Copy the new DLL (libMindfulTrader.dll is copied and renamed to MindfulTrader.dll)
echo "Copying new compiled DLL..."
if cp "$SOURCE_FILE" "$DEST_FILE"; then
    echo "   -> Main DLL copied successfully."
else
    echo "Error: Failed to copy the new DLL. Check write permissions."
    exit 1
fi

# 5. Copy necessary runtime dependencies
echo ""
echo "Copying essential dynamically linked runtime dependencies..."

# Function to copy a dependency from a given source path
copy_dependency() {
    local dep_name="$1"
    local source_path="$2"
    local dep_source="$source_path/$dep_name"
    local dep_dest="$DEST_DIR/$dep_name"

    if [ -f "$dep_source" ]; then
        if [ ! -f "$dep_dest" ] || ! cmp -s "$dep_source" "$dep_dest"; then
            cp "$dep_source" "$dep_dest"
            echo "   -> Copied $dep_name"
        else
            echo "   -> $dep_name already up-to-date. Skipped."
        fi
    else
        echo "   -> Warning: Dependency file not found: $dep_source"
    fi
}

echo ""
echo "✅ DEPLOYMENT COMPLETE"
echo "New DLL and critical dependencies deployed to: $DEST_DIR"
echo "Your DLL should now load as it contains the required static C/C++ runtimes."

exit 0
