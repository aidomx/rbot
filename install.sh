#!/bin/sh
# install.sh — pasang rbot.
#
# Pemakaian:
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh -s -- --dev
#   ./install.sh --dev  (jika dijalankan dari dalam source tree lokal)
#
# Mode default (curl) mengunduh binary rbot yang sudah di-build ke /usr/local/bin/rbot.
# Mode --dev melakukan bootstrap build rbot dari source dengan compiler C lokal:
# dari source tree lokal hasilnya disimpan di build/bin/rbot; bila source
# diunduh sementara (curl | sh -s -- --dev), binary dipasang ke path instalasi
# sebelum temp source terhapus.

set -eu

GITHUB_USER="aidomx"
REPO_NAME="rbot"
BRANCH="main"
BIN_PATH_IN_REPO="bin/rbot"
VERSION="v0.1.8"

# Lokasi instalasi default. Termux memakai $PREFIX/bin.
case "$(uname -s 2>/dev/null || echo unknown)" in
  Darwin)
    INSTALL_PATH="/usr/local/bin/rbot"
    ;;
  Linux)
    if [ -n "${PREFIX:-}" ] && [ -d "${PREFIX}" ]; then
      INSTALL_PATH="${PREFIX}/bin/rbot"
    else
      INSTALL_PATH="/usr/local/bin/rbot"
    fi
    ;;
  *)
    INSTALL_PATH="/usr/local/bin/rbot"
    ;;
esac

read_version() {
  file="$1/.rbot-version"
  if [ -f "$file" ]; then
    v=$(sed -n "1p" "$file")
    [ -n "$v" ] && VERSION="$v"
  fi
}

RAW_URL="https://raw.githubusercontent.com/${GITHUB_USER}/${REPO_NAME}/${BRANCH}/${BIN_PATH_IN_REPO}"
SOURCE_URL="https://github.com/${GITHUB_USER}/${REPO_NAME}/archive/refs/heads/${BRANCH}.tar.gz"

# Fungsi instalasi global (hanya digunakan untuk mode default / curl)
install_binary() {
  binary="$1"
  chmod +x "$binary"
  echo "> Installing : ${INSTALL_PATH}"

  install_dir="$(dirname "${INSTALL_PATH}")"

  if [ -w "$install_dir" ]; then
    mv "$binary" "${INSTALL_PATH}"
  elif command -v sudo >/dev/null 2>&1; then
    sudo mv "$binary" "${INSTALL_PATH}"
  else
    echo "rbot: tidak punya izin tulis ke ${install_dir} dan sudo tidak tersedia" >&2
    return 1
  fi

  echo "> Installed  : ${INSTALL_PATH}"
  if command -v "${INSTALL_PATH}" >/dev/null 2>&1; then
    "${INSTALL_PATH}" version 2>/dev/null || true
  fi
}

if [ "${1:-}" = "--dev" ]; then
  TMP_DIR="$(mktemp -d)"
  cleanup() { rm -rf "$TMP_DIR"; }
  trap cleanup EXIT INT TERM

  SRC_DIR=""
  
  # Deteksi apakah dijalankan dari dalam source tree lokal
  SCRIPT_DIR="$(cd "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
  if [ -f "${SCRIPT_DIR}/src/main.c" ] && [ -f "${SCRIPT_DIR}/include/rbot.h" ]; then
    SRC_DIR="${SCRIPT_DIR}"
  else
    command -v curl >/dev/null 2>&1 || {
      echo "rbot: --dev membutuhkan curl untuk mengunduh source" >&2
      exit 1
    }
    command -v tar >/dev/null 2>&1 || {
      echo "rbot: --dev membutuhkan tar untuk mengekstrak source" >&2
      exit 1
    }

    echo "> Downloading source: ${SOURCE_URL}"
    curl -fsSL "${SOURCE_URL}" -o "${TMP_DIR}/rbot.tar.gz"
    tar -xzf "${TMP_DIR}/rbot.tar.gz" -C "${TMP_DIR}"
    SRC_DIR=$(find "${TMP_DIR}" -mindepth 1 -maxdepth 1 -type d | head -n 1)

    if [ -z "${SRC_DIR}" ] || [ ! -f "${SRC_DIR}/src/main.c" ]; then
      echo "rbot: source tree tidak valid" >&2
      exit 1
    fi
  fi

  read_version "${SRC_DIR}"

  command -v "${CC:-cc}" >/dev/null 2>&1 || {
    echo "rbot: compiler C '${CC:-cc}' tidak ditemukan; pasang gcc/clang terlebih dahulu" >&2
    exit 1
  }

  echo "> Bootstrap  : rbot ${VERSION}"
  echo "> Compiler   : ${CC:-cc}"

  mkdir -p "${TMP_DIR}/build/obj"
  SOURCES=$(find "${SRC_DIR}/src" -name '*.c' -type f | sort)
  TOTAL=$(printf '%s\n' "$SOURCES" | sed '/^$/d' | wc -l | tr -d ' ')
  
  if [ "$TOTAL" -eq 0 ]; then
    echo "rbot: tidak ada file sumber (.c) ditemukan di ${SRC_DIR}/src" >&2
    exit 1
  fi
  
  DONE=0

  progress() {
    percent="$1"
    cols="${COLUMNS:-}"

    if [ -z "$cols" ] && command -v tput >/dev/null 2>&1; then
      cols=$(tput cols 2>/dev/null || true)
    fi
    case "$cols" in
      ''|*[!0-9]*) cols=24 ;;
    esac

    width=$((cols - 7))
    [ "$width" -lt 6 ] && width=6
    [ "$width" -gt 40 ] && width=40

    filled=$((percent * width / 100))
    empty=$((width - filled))
    bar=$(printf '%*s' "$filled" '' | tr ' ' '_')
    rest=$(printf '%*s' "$empty" '' | tr ' ' '.')

    if [ -t 1 ]; then
      green='\033[32m'
      dark='\033[90m'
      reset='\033[0m'
      printf '\r> %b%s%b%s%b %d%%' "$green" "$bar" "$dark" "$rest" "$reset" "$percent"
    else
      printf '\r> %s%s %d%%' "$bar" "$rest" "$percent"
    fi
  }

  compile_one() {
    src="$1"
    obj="${TMP_DIR}/build/obj/${src#${SRC_DIR}/src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra \
      -DRBOT_VERSION_EMBEDDED=\"${VERSION}\" \
      -I"${SRC_DIR}/include" -I"${SRC_DIR}/src" \
      -c "$src" -o "$obj"
    DONE=$((DONE + 1))
    progress $((DONE * 90 / TOTAL))
  }

  for src in $SOURCES; do
    compile_one "$src"
  done

  OBJECTS=$(find "${TMP_DIR}/build/obj" -name '*.o' -type f | sort)
  
  # PERBAIKAN: Output binary ke build/bin/rbot di dalam direktori source (lokal)
  mkdir -p "${SRC_DIR}/build/bin"
  "${CC:-cc}" $OBJECTS -o "${SRC_DIR}/build/bin/rbot" -pthread -lm
  progress 100
  printf '\n'

  chmod +x "${SRC_DIR}/build/bin/rbot"
  echo "> Built & Saved: ${SRC_DIR}/build/bin/rbot"

  # Cek versi jika berhasil di-build
  if [ -x "${SRC_DIR}/build/bin/rbot" ]; then
    "${SRC_DIR}/build/bin/rbot" version 2>/dev/null || true
  fi

  # Bila source berasal dari unduhan sementara (di dalam TMP_DIR), binary
  # wajib dipindah keluar sebelum trap cleanup menghapus TMP_DIR saat exit.
  case "${SRC_DIR}" in
    "${TMP_DIR}"/*)
      if ! install_binary "${SRC_DIR}/build/bin/rbot"; then
        cp "${SRC_DIR}/build/bin/rbot" ./rbot &&
          echo "> Saved (fallback): $PWD/rbot"
      fi
      ;;
  esac

  exit 0
fi

# --- Mode Default (Curl / Unduh Binary) ---
TMP_FILE="$(mktemp)"
cleanup() { rm -f "${TMP_FILE}"; }
trap cleanup EXIT INT TERM

echo "> Downloading: ${RAW_URL}"

if ! curl -fsSL "${RAW_URL}" -o "${TMP_FILE}"; then
  echo "rbot: gagal mengunduh binary dari ${RAW_URL}" >&2
  exit 1
fi

install_binary "${TMP_FILE}"
# Trap cleanup akan otomatis menghapus TMP_FILE saat skrip selesai

