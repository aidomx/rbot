#!/bin/sh
# rbot installation script
#
# This is a standard, safe build/install script for the rbot C project.
# It compiles the source code and places the binary in the bin/ directory.
#
# Pemakaian:
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh -s -- --dev
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh -s -- --dev --version v0.2.0
#   curl -fsSL https://raw.githubusercontent.com/aidomx/rbot/main/install.sh | sh -s -- --version v0.2.0
#   ./install.sh --dev  (jika dijalankan dari dalam source tree lokal)
#
# Mode default (curl) mengunduh binary rbot dari GitHub Releases ke /usr/local/bin/rbot.
# Mode --dev melakukan bootstrap build rbot dari source dengan compiler C lokal.
#
# Sumber versi (urutan prioritas):
#   1. --version <tag>    (argumen eksplisit)
#   2. $RBOT_VERSION      (env, di-set CI dari git tag, mis. v0.2.0)
#   3. .rbot-version      (file di source tree; hanya untuk --dev)
#   4. fallback hardcoded

set -eu

GITHUB_USER="aidomx"
REPO_NAME="rbot"
BRANCH="main"
VERSION="v0.2.0"   # fallback terakhir; ditimpa oleh --version / RBOT_VERSION / .rbot-version

# Nama asset di GitHub Releases (harus persis sama dengan yang
# di-upload oleh .github/workflows/release.yml).
ASSET_WINDOWS_X64="rbot-windows-x64.exe"
ASSET_LINUX_X64="rbot-linux-x64"
ASSET_LINUX_ARM64="rbot-linux-arm64"
ASSET_MACOS_ARM64="rbot-macos-arm64"
ASSET_MACOS_X64="rbot-macos-x64"

# ---------------------------------------------------------------------------
# Parsing argumen
# ---------------------------------------------------------------------------
DEV=0
VERSION_TAG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dev)
      DEV=1
      shift
      ;;
    --version)
      if [ $# -lt 2 ]; then
        echo "rbot: --version memerlukan argumen tag (mis. --version v0.2.0)" >&2
        exit 1
      fi
      VERSION_TAG="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

# ---------------------------------------------------------------------------
# Lokasi instalasi default. Termux memakai $PREFIX/bin.
# ---------------------------------------------------------------------------
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

# Prioritas: RBOT_VERSION (env) > .rbot-version (file). VERSION_TAG sudah
# ditangani di pemanggil sebelum fungsi ini dipanggil.
read_version() {
  # Prioritas 1: env RBOT_VERSION (di-set CI dari git tag v*).
  if [ -n "${RBOT_VERSION:-}" ]; then
    VERSION="$RBOT_VERSION"
    return
  fi

  # Prioritas 2: file .rbot-version (untuk dev lokal).
  file="$1/.rbot-version"
  if [ -f "$file" ]; then
    v=$(sed -n "1p" "$file")
    [ -n "$v" ] && VERSION="$v"
  fi
}

# ---------------------------------------------------------------------------
# Installer
# ---------------------------------------------------------------------------
install_binary() {
  binary="$1"
  chmod +x "$binary"
  echo "> Installing : ${INSTALL_PATH}"

  install_dir="$(dirname "${INSTALL_PATH}")"

  if [ ! -d "$install_dir" ]; then
    if command -v sudo >/dev/null 2>&1; then
      sudo mkdir -p "$install_dir"
    else
      mkdir -p "$install_dir" || {
        echo "rbot: tidak bisa membuat ${install_dir}" >&2
        return 1
      }
    fi
  fi

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

# ---------------------------------------------------------------------------
# Helper untuk mode default (GitHub Releases)
# ---------------------------------------------------------------------------
detect_asset_name() {
  os="$(uname -s 2>/dev/null || echo unknown)"
  arch="$(uname -m 2>/dev/null || echo unknown)"

  case "$os" in
    Linux)
      case "$arch" in
        x86_64|amd64)   echo "$ASSET_LINUX_X64" ;;
        aarch64|arm64)  echo "$ASSET_LINUX_ARM64" ;;
        *) echo "rbot: arsitektur Linux tidak didukung: $arch" >&2; return 1 ;;
      esac
      ;;
    Darwin)
      case "$arch" in
        arm64)  echo "$ASSET_MACOS_ARM64" ;;
        x86_64) echo "$ASSET_MACOS_X64" ;;
        *) echo "rbot: arsitektur macOS tidak didukung: $arch" >&2; return 1 ;;
      esac
      ;;
    MINGW*|MSYS*|CYGWIN*)
      echo "$ASSET_WINDOWS_X64"
      ;;
    *)
      echo "rbot: OS tidak didukung: $os" >&2
      return 1
      ;;
  esac
}

# Ambil JSON metadata release (latest atau tag tertentu).
fetch_release_json() {
  tag="$1"
  if [ -n "$tag" ]; then
    api="https://api.github.com/repos/${GITHUB_USER}/${REPO_NAME}/releases/tags/${tag}"
  else
    api="https://api.github.com/repos/${GITHUB_USER}/${REPO_NAME}/releases/latest"
  fi

  curl -fsSL \
    -H 'User-Agent: rbot-installer' \
    -H 'Accept: application/vnd.github+json' \
    "$api"
}

# Parse field JSON satu baris dengan split per-koma.
# Contoh: json_field "$json" '"tag_name"'  →  v0.2.0
json_field() {
  json="$1"
  key="$2"
  printf '%s' "$json" \
    | tr ',' '\n' \
    | grep -F "$key" \
    | head -n1 \
    | sed -E 's/.*:"([^"]+)".*/\1/'
}

# ---------------------------------------------------------------------------
# Mode --dev: bootstrap build dari source
# ---------------------------------------------------------------------------
if [ "$DEV" -eq 1 ]; then
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

    SOURCE_URL="https://github.com/${GITHUB_USER}/${REPO_NAME}/archive/refs/heads/${BRANCH}.tar.gz"
    echo "> Downloading source: ${SOURCE_URL}"
    curl -fsSL "${SOURCE_URL}" -o "${TMP_DIR}/rbot.tar.gz"
    tar -xzf "${TMP_DIR}/rbot.tar.gz" -C "${TMP_DIR}"
    SRC_DIR=$(find "${TMP_DIR}" -mindepth 1 -maxdepth 1 -type d | head -n 1)

    if [ -z "${SRC_DIR}" ] || [ ! -f "${SRC_DIR}/src/main.c" ]; then
      echo "rbot: source tree tidak valid" >&2
      exit 1
    fi
  fi

  # Prioritas versi:
  #   1. --version <tag>
  #   2. RBOT_VERSION (env, di-set CI dari git tag)
  #   3. .rbot-version
  #   4. $VERSION (fallback hardcoded)
  if [ -n "${VERSION_TAG}" ]; then
    VERSION="${VERSION_TAG}"
  else
    read_version "${SRC_DIR}"
  fi

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

  mkdir -p "${SRC_DIR}/build/bin"
  "${CC:-cc}" $OBJECTS -o "${SRC_DIR}/build/bin/rbot" -pthread -lm
  progress 100
  printf '\n'

  chmod +x "${SRC_DIR}/build/bin/rbot"
  echo "> Built & Saved: ${SRC_DIR}/build/bin/rbot"

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

# ---------------------------------------------------------------------------
# Mode default: unduh binary dari GitHub Releases
# ---------------------------------------------------------------------------

command -v curl >/dev/null 2>&1 || {
  echo "rbot: membutuhkan curl untuk mengunduh binary" >&2
  exit 1
}

# Di mode default, --version eksplisit menang; kalau tidak, RBOT_VERSION dipakai;
# kalau tidak ada keduanya, pakai release "latest".
if [ -z "${VERSION_TAG}" ] && [ -n "${RBOT_VERSION:-}" ]; then
  VERSION_TAG="${RBOT_VERSION}"
fi

ASSET="$(detect_asset_name)" || exit 1

echo "> Platform   : ${ASSET}"
if [ -n "${VERSION_TAG}" ]; then
  echo "> Requesting : tag ${VERSION_TAG}"
else
  echo "> Requesting : latest release"
fi

RELEASE_JSON="$(fetch_release_json "${VERSION_TAG}")" || {
  echo "rbot: gagal mengambil metadata release dari GitHub API" >&2
  exit 1
}

TAG_NAME="$(json_field "${RELEASE_JSON}" '"tag_name"')"
if [ -n "${TAG_NAME}" ]; then
  echo "> Release    : ${TAG_NAME}"
fi

# Ambil URL browser_download_url untuk asset yang cocok.
URL="$(printf '%s' "${RELEASE_JSON}" \
  | tr ',' '\n' \
  | grep -F 'https://' \
  | grep -F "/${ASSET}\"" \
  | head -n1 \
  | sed -E 's/.*"(https[^"]+)".*/\1/')"

if [ -z "${URL}" ]; then
  echo "rbot: asset '${ASSET}' tidak ada di release ${TAG_NAME:-latest}" >&2
  echo "rbot: asset yang tersedia:" >&2
  printf '%s' "${RELEASE_JSON}" \
    | tr ',' '\n' \
    | grep -F '"name"' \
    | sed 's/^/  /' >&2
  exit 1
fi

TMP_FILE="$(mktemp)"
cleanup() { rm -f "${TMP_FILE}"; }
trap cleanup EXIT INT TERM

echo "> Downloading: ${URL}"
if ! curl -fsSL "${URL}" -o "${TMP_FILE}"; then
  echo "rbot: gagal mengunduh binary dari ${URL}" >&2
  exit 1
fi

install_binary "${TMP_FILE}"
# Trap cleanup akan otomatis menghapus TMP_FILE saat skrip selesai
