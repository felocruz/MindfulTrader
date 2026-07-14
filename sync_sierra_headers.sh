#!/bin/bash
# Sync Sierra Chart header files to local dependencies folder
# Run this script after updating Sierra Chart to sync API changes

SIERRA_SOURCE="/mnt/c/SierraChart2/ACS_Source"
LOCAL_DEPS="sierra_chart_dependencies"

echo "🔄 Syncing Sierra Chart headers..."
echo "   Source: $SIERRA_SOURCE"
echo "   Destination: $LOCAL_DEPS"
echo ""

if [ ! -d "$SIERRA_SOURCE" ]; then
    echo "❌ ERROR: Sierra Chart source directory not found at $SIERRA_SOURCE"
    echo "   Make sure Sierra Chart is installed at C:\\SierraChart2"
    exit 1
fi

# Count files before sync
BEFORE_COUNT=$(ls -1 "$LOCAL_DEPS"/*.h 2>/dev/null | wc -l)

# Sync headers (rsync only copies changed files)
rsync -av --delete "$SIERRA_SOURCE"/*.h "$LOCAL_DEPS/"

if [ $? -eq 0 ]; then
    AFTER_COUNT=$(ls -1 "$LOCAL_DEPS"/*.h 2>/dev/null | wc -l)
    echo ""
    echo "✅ Headers synced successfully!"
    echo "   Total headers: $AFTER_COUNT"
    
    if [ $AFTER_COUNT -ne $BEFORE_COUNT ]; then
        echo "   ⚠️  File count changed: $BEFORE_COUNT → $AFTER_COUNT"
        echo "   Rebuild required!"
    fi
    
    # Update reference copy
    if [ -f "sierra_chart_dependencies/sierrachart.h" ]; then
        cp sierra_chart_dependencies/sierrachart.h acs_source_sierrachart.h
        echo "   📄 Reference copy updated: acs_source_sierrachart.h"
    fi
else
    echo ""
    echo "❌ Sync failed!"
    exit 1
fi
