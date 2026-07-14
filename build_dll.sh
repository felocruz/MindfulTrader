#!/bin/bash
#
# MindfulTrader DLL Build Script
# Elite v2.4 Triple-Lock - Clean Build with PCH Regeneration
#
# Usage: ./build_dll.sh [--clean-only] [--no-clean] [--jobs N]
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default settings
BUILD_DIR="build-windows"
CLEAN=true
BUILD=true
JOBS=$(nproc)

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean-only)
            BUILD=false
            shift
            ;;
        --no-clean)
            CLEAN=false
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --clean-only    Only clean build files, don't build"
            echo "  --no-clean      Skip cleaning, just rebuild"
            echo "  --jobs N        Use N parallel jobs (default: $(nproc))"
            echo "  -h, --help      Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  MindfulTrader DLL Build Script${NC}"
echo -e "${BLUE}  Elite v2.4 Triple-Lock (int8_t enums + int8 FlatBuffers)${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}[0/3] Initializing build directory...${NC}"
    if ! cmake --preset wsl-clang-cl-release 2>&1 | tail -20; then
        echo -e "${RED}✗ Initial CMake configuration failed${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Build directory initialized${NC}"
    echo ""
fi

cd "$BUILD_DIR"

# ============================================================================
# Phase 1: Clean Build Files
# ============================================================================
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}[1/3] Cleaning old build files...${NC}"
    
    # Run ninja clean
    if [ -f "build.ninja" ]; then
        ninja clean
        echo -e "${GREEN}✓ Cleaned build artifacts${NC}"
    else
        echo -e "${YELLOW}⚠ No build.ninja found, skipping ninja clean${NC}"
    fi
    
    # Remove PCH files explicitly (forces regeneration)
    PCH_FILES=$(find . -name "*.gch" -o -name "*.pch" -o -name "*cmake_pch*" 2>/dev/null)
    if [ -n "$PCH_FILES" ]; then
        echo "$PCH_FILES" | while read -r pch_file; do
            rm -f "$pch_file"
            echo -e "${GREEN}✓ Removed PCH: $pch_file${NC}"
        done
    fi
    
    # Remove CMake cache to force complete reconfiguration
    if [ -f "CMakeCache.txt" ]; then
        rm -f CMakeCache.txt
        echo -e "${GREEN}✓ Removed CMakeCache.txt${NC}"
    fi
    
    # Reconfigure CMake to regenerate build files
    echo -e "${YELLOW}Reconfiguring CMake...${NC}"
    cd ..
    if ! cmake --preset wsl-clang-cl-release 2>&1 | tail -20; then
        echo -e "${RED}✗ CMake configuration failed${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ CMake reconfigured${NC}"
    
    # Verify build directory was created properly
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        echo -e "${RED}✗ CMake did not generate build.ninja${NC}"
        exit 1
    fi
    cd "$BUILD_DIR"
    
    echo ""
fi

# ============================================================================
# Phase 2: Build DLL
# ============================================================================
if [ "$BUILD" = true ]; then
    echo -e "${YELLOW}[2/3] Building MindfulTrader.dll (${JOBS} parallel jobs)...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
    
    # Capture start time
    START_TIME=$(date +%s)
    
    # Create a named pipe for build output processing
    BUILD_LOG="build_output.log"
    > "$BUILD_LOG"
    
    # Build and process output in real-time
    cmake --build . -- -j"$JOBS" 2>&1 | while IFS= read -r line; do
        # Log everything to file
        echo "$line" >> "$BUILD_LOG"
        
        # Parse and display different types of output
        if [[ "$line" =~ ^\[([0-9]+)/([0-9]+)\] ]]; then
            # Progress line: [N/M] Building file...
            current="${BASH_REMATCH[1]}"
            total="${BASH_REMATCH[2]}"
            percent=$((current * 100 / total))
            
            # Extract filename (everything after the closing bracket)
            filename="${line#*] }"
            # Truncate long filenames
            if [ ${#filename} -gt 80 ]; then
                filename="${filename:0:77}..."
            fi
            
            printf "\r${BLUE}[%3d%%]${NC} %s" "$percent" "$filename"
            
        elif [[ "$line" =~ error: ]]; then
            # Error line - highlight in red
            echo -e "\r${RED}ERROR: $line${NC}"
            
        elif [[ "$line" =~ warning: ]]; then
            # Warning line - highlight in yellow
            echo -e "\r${YELLOW}⚠ WARNING: $line${NC}"
            
        elif [[ "$line" =~ "Build files have been written" ]]; then
            echo -e "\r${GREEN}✓ CMake configuration complete${NC}"
            
        elif [[ "$line" =~ "ninja: build stopped" ]]; then
            echo -e "\r${RED}✗ Build stopped: $line${NC}"
            
        fi
    done
    
    BUILD_EXIT_CODE=${PIPESTATUS[0]}
    END_TIME=$(date +%s)
    BUILD_TIME=$((END_TIME - START_TIME))
    
    echo ""
    echo ""
    
    # Check for build failure
    if [ $BUILD_EXIT_CODE -ne 0 ]; then
        echo -e "${RED}✗ Build failed (exit code: $BUILD_EXIT_CODE)${NC}"
        echo ""
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${YELLOW}Errors and Warnings:${NC}"
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        grep -E "(error:|warning:|note:)" "$BUILD_LOG" | head -50
        echo ""
        echo -e "${YELLOW}Full Build Log:${NC}"
        echo -e "${YELLOW}cat $BUILD_DIR/$BUILD_LOG${NC}"
        echo ""
        exit 1
    fi
    
    echo ""
    echo -e "${GREEN}✓ Build completed in ${BUILD_TIME}s${NC}"
    echo ""
    
    # Check if DLL exists and show info
    DLL_PATH="bin/MindfulTrader.dll"
    if [ -f "$DLL_PATH" ]; then
        # Get file size (works on both Linux and macOS)
        if command -v stat &> /dev/null; then
            if [[ "$OSTYPE" == "darwin"* ]]; then
                DLL_SIZE=$(stat -f%z "$DLL_PATH")
                DLL_DATE=$(stat -f%Sm -t "%Y-%m-%d %H:%M:%S" "$DLL_PATH")
            else
                DLL_SIZE=$(stat -c%s "$DLL_PATH")
                DLL_DATE=$(stat -c%y "$DLL_PATH" | cut -d' ' -f1-2)
            fi
        else
            DLL_SIZE=$(wc -c < "$DLL_PATH")
            DLL_DATE=$(date -r "$DLL_PATH" "+%Y-%m-%d %H:%M:%S")
        fi
        # Convert to MB using awk (no bc dependency)
        DLL_SIZE_MB=$(awk "BEGIN {printf \"%.2f\", $1/1024/1024}" <<< "$DLL_SIZE")
        
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${GREEN}  Build Successful! 🎉${NC}"
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo ""
        echo -e "  📦 DLL Path:     ${YELLOW}$DLL_PATH${NC}"
        echo -e "  📊 Size:         ${YELLOW}${DLL_SIZE_MB} MB${NC}"
        echo -e "  📅 Timestamp:    ${YELLOW}$DLL_DATE${NC}"
        echo ""
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    else
        echo -e "${RED}✗ Error: DLL not found at $DLL_PATH${NC}"
        exit 1
    fi
fi

# ============================================================================
# Phase 3: Deployment Instructions
# ============================================================================
if [ "$BUILD" = true ] && [ -f "bin/MindfulTrader.dll" ]; then
    echo ""
    echo -e "${YELLOW}[3/3] Next Steps:${NC}"
    echo ""
    echo -e "  ${BLUE}1.${NC} Deploy to Sierra Chart:"
    echo -e "     ${YELLOW}./deploy_mindfultrader.sh${NC}"
    echo ""
    echo -e "  ${BLUE}2.${NC} Verify int8 enum storage:"
    echo -e "     ${YELLOW}# Collect new .lbr data in Sierra Chart${NC}"
    echo -e "     ${YELLOW}# File size should be ~10-12% smaller${NC}"
    echo ""
    echo -e "  ${BLUE}3.${NC} Test Python pipeline:"
    echo -e "     ${YELLOW}cd ../lbrnet${NC}"
    echo -e "     ${YELLOW}mamba run -n mts python scripts/collect_data.py \\${NC}"
    echo -e "     ${YELLOW}  --input=data/raw/new_int8_data.lbr \\${NC}"
    echo -e "     ${YELLOW}  --output=data/training/test.lbr \\${NC}"
    echo -e "     ${YELLOW}  --max-events=1000${NC}"
    echo ""
fi

echo -e "${GREEN}✓ Script completed successfully${NC}"
echo ""
