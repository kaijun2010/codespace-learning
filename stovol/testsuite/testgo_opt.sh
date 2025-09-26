#!/bin/bash

# testgo.sh - Test script for stovol program
# Usage: ./testgo.sh [MODE]
# MODE: r (recovery mode) or c (close mode)

# Set up environment variables
export TRADE_KIND=0             # 0:日盤, 1:夜盤
export oLOG=.                   # 測試環境日誌目錄 (本目錄)
export LOG=$oLOG                # 統一使用 LOG (程式使用 build_log_path() 時未設定 STOVOL_LOG_DIR 會回落到 .)
export ETL_PATH=/tmp/stovol_etl # ETL directory
export NFS_PATH=/tmp/stovol_nfs # NFS directory
export NFS_PATH_T=/tmp/stovol_nfs_t # NFS_T directory
export ETL_DB_SECOND_PATH=/tmp/stovol_etl_second # ETL second directory
#export STOVOL_DEBUG_PGSEQ=123
#export STOVOL_DEBUG_LEVELS=5


# Create directories if they don't exist
mkdir -p "$LOG"
mkdir -p $ETL_PATH
mkdir -p $NFS_PATH
mkdir -p $NFS_PATH_T
mkdir -p $ETL_DB_SECOND_PATH

# Program parameters
GRPID=1
LLM_CONFIG_FILE="../config/llm_sirs.ini"
OPT_DBDATA_SQLITE="../data/opt/db_data.datDB_ORG"
FUT_DBDATA_SQLITE="../data/fut/db_data.datDB_ORG"
OPT_MATCH_FILE="../data/opt/match_p.dat"
FUT_MATCH_FILE="../data/fut/match_p.dat"
OPT_TPC_FILE="../data/opt/tpc.db"


# Get MODE from command line argument
MODE=${1:-""}

echo "==========================================="
echo "STOVOL Test Script"
echo "==========================================="
echo "Environment Variables:"
#echo "PRODUCT_TYPE: $PRODUCT_TYPE"
echo "TRADE_KIND: $TRADE_KIND"
echo "LOG: $LOG"
echo "ETL_PATH: $ETL_PATH"
echo "NFS_PATH: $NFS_PATH"
echo "NFS_PATH_T: $NFS_PATH_T"
echo "ETL_DB_SECOND_PATH: $ETL_DB_SECOND_PATH"
echo "OPT_TPC_FILE: $OPT_TPC_FILE"
echo "==========================================="
echo "Program Parameters:"
echo "GRPID: $GRPID"
echo "LLM_CONFIG_FILE: $LLM_CONFIG_FILE"
echo "OPT_DBDATA_SQLITE: $OPT_DBDATA_SQLITE"
echo "FUT_DBDATA_SQLITE: $FUT_DBDATA_SQLITE"
echo "OPT_MATCH_FILE: $OPT_MATCH_FILE"
echo "FUT_MATCH_FILE: $FUT_MATCH_FILE"
echo "OPT_TPC_FILE: $OPT_TPC_FILE"
echo "==========================================="

# Create dummy files for testing
# echo "Creating test files..."
# touch "$LLM_CONFIG_FILE"
# touch "$OPT_DBDATA_SQLITE"
# touch "$FUT_DBDATA_SQLITE"
# touch "$OPT_MATCH_FILE"
# touch "$FUT_MATCH_FILE"

# Check if required files exist
echo "Checking required files..."
MISSING_FILES=()

if [ ! -f "$LLM_CONFIG_FILE" ]; then
    MISSING_FILES+=("$LLM_CONFIG_FILE")
fi

if [ ! -f "$OPT_DBDATA_SQLITE" ]; then
    MISSING_FILES+=("$OPT_DBDATA_SQLITE")
fi

if [ ! -f "$FUT_DBDATA_SQLITE" ]; then
    MISSING_FILES+=("$FUT_DBDATA_SQLITE")
fi

if [ ! -f "$OPT_MATCH_FILE" ]; then
    MISSING_FILES+=("$OPT_MATCH_FILE")
fi

if [ ! -f "$FUT_MATCH_FILE" ]; then
    MISSING_FILES+=("$FUT_MATCH_FILE")
fi

if [ ! -f "$OPT_TPC_FILE" ]; then
    MISSING_FILES+=("$OPT_TPC_FILE")
fi

# Check if any files are missing
if [ ${#MISSING_FILES[@]} -gt 0 ]; then
    echo "==========================================="
    echo "ERROR: Required files are missing!"
    echo "==========================================="
    for file in "${MISSING_FILES[@]}"; do
        echo "Missing file: $file"
    done
    echo "==========================================="
    echo "Please create the missing files before running the test."
    echo "Program execution aborted."
    echo "==========================================="
    exit 1
fi

echo "All required files exist. Proceeding with test..."

# Build command line
STOVOL_CMD="../stovol"
#STOVOL_CMD="../stovol_ut"

# Add mode options
case "$MODE" in
    "r")
        STOVOL_CMD="$STOVOL_CMD -r"
        echo "Running in RECOVERY mode (-r)"
        ;;
    "c")
        STOVOL_CMD="$STOVOL_CMD -c"
        echo "Running in CLOSE mode (-c)"
        ;;
    "")
        echo "Running in NORMAL mode"
        ;;
    *)
        echo "Invalid mode: $MODE"
        echo "Usage: $0 [r|c]"
        echo "  r: recovery mode"
        echo "  c: close mode"
        echo "  (no parameter): normal mode"
        exit 1
        ;;
esac

# RM log
RMLOG_CMD="/usr/bin/rm -rf *.log"
echo "==========================================="
echo "Executing command:"
echo "$RMLOG_CMD"
$RMLOG_CMD

# Add parameters
STOVOL_CMD="$STOVOL_CMD -o $GRPID $LLM_CONFIG_FILE $OPT_DBDATA_SQLITE $OPT_MATCH_FILE $OPT_TPC_FILE"

echo "==========================================="
echo "Executing command:"
echo "$STOVOL_CMD"
echo "==========================================="

# Execute the program
$STOVOL_CMD

echo "==========================================="
echo "Program execution completed"
echo "==========================================="
