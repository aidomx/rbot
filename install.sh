#!/bin/sh
# install.sh — pasang binary rbot hasil build dari repo GitHub ke /usr/bin/rbot.
#
# Pemakaian:
#   curl -fsSL https://raw.githubusercontent.com/<user>/rbot/main/install.sh | sh
#
# Script ini HANYA mengunduh binary rbot yang sudah di-build dan disimpan
# di dalam repo (bukan build dari source), lalu memasangnya ke /usr/bin/rbot.

set -eu

# ---- konfigurasi ----
GITHUB_USER="aidomx"
REPO_NAME="rbot"
BRANCH="main"
BIN_PATH_IN_REPO="bin/rbot"     # lokasi binary hasil build di dalam repo
INSTALL_PATH="/usr/bin/rbot"
# ----------------------

RAW_URL="https://raw.githubusercontent.com/${GITHUB_USER}/${REPO_NAME}/${BRANCH}/${BIN_PATH_IN_REPO}"

echo "> Downloading: ${RAW_URL}"

TMP_FILE="$(mktemp)"
cleanup() { rm -f "${TMP_FILE}"; }
trap cleanup EXIT INT TERM

if ! curl -fsSL "${RAW_URL}" -o "${TMP_FILE}"; then
  echo "rbot: gagal mengunduh binary dari ${RAW_URL}" >&2
  exit 1
fi

chmod +x "${TMP_FILE}"

echo "> Installing : ${INSTALL_PATH}"

if [ -w "$(dirname "${INSTALL_PATH}")" ]; then
  mv "${TMP_FILE}" "${INSTALL_PATH}"
elif command -v sudo >/dev/null 2>&1; then
  sudo mv "${TMP_FILE}" "${INSTALL_PATH}"
else
  echo "rbot: tidak punya izin tulis ke $(dirname "${INSTALL_PATH}") dan sudo tidak tersedia" >&2
  exit 1
fi

trap - EXIT INT TERM

echo "> Installed  : ${INSTALL_PATH}"
"${INSTALL_PATH}" version 2>/dev/null || true

