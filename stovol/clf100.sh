#!/usr/bin/env bash
# Format C++ sources/headers with clang-format (LLVM base, ColumnLimit=100)
# Project root is assumed to be the directory containing this script.

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$ROOT_DIR/src"
INC_DIR="$ROOT_DIR/include"
STYLE='{BasedOnStyle: LLVM, ColumnLimit: 100}'

# Check clang-format availability
if ! command -v clang-format >/dev/null 2>&1; then
  echo "ERROR: clang-format 未安裝或不可執行，請先安裝後再試。" >&2
  exit 1
fi

format_dir() {
  local dir="$1"
  local pattern="$2"
  if [[ -d "$dir" ]]; then
    # 使用 -exec … + 會自動批次處理且在無檔案時不會出錯
    find "$dir" -type f -name "$pattern" -exec clang-format -i -style="$STYLE" {} +
    # 顯示格式化檔案數
    local cnt
    cnt=$(find "$dir" -type f -name "$pattern" | wc -l | awk '{print $1}')
    echo "Formatted $cnt file(s) in $dir matching $pattern"
  else
    echo "Skip: 目錄不存在：$dir"
  fi
}

# 對 src/*.cpp 與 include/*.h 進行格式化
format_dir "$SRC_DIR" "*.cpp"
format_dir "$INC_DIR" "*.h"

echo "clang-format 完成（風格：$STYLE）"
