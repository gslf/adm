#!/bin/sh
# adm installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/gslf/adm/main/install.sh | sh
#
# Options (flags after `sh -s --`, or environment variables):
#   --version <tag>     ADM_VERSION        release tag to install (default: latest)
#   --dir <path>        ADM_INSTALL_DIR    where the release files live
#                                          (default: ${XDG_DATA_HOME:-~/.local/share}/adm)
#   --bin-dir <path>    ADM_BIN_DIR        where the `adm` link is created (default: ~/.local/bin)
#   --no-modify-path    ADM_NO_MODIFY_PATH=1  do not touch shell startup files
#
# ADM_DOWNLOAD_BASE overrides the URL the release assets are fetched from
# (used by CI to test unpublished builds).

set -eu

REPO="gslf/adm"

say() { printf 'adm-install: %s\n' "$*"; }
die() { printf 'adm-install: error: %s\n' "$*" >&2; exit 1; }
has() { command -v "$1" >/dev/null 2>&1; }

download() { # url dest
  if has curl; then
    curl -fsSL --retry 3 -o "$2" "$1"
  elif has wget; then
    wget -q -O "$2" "$1"
  else
    die "curl or wget is required"
  fi
}

latest_version() {
  if has curl; then
    url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") || return 1
    printf '%s\n' "${url##*/}"
  else
    wget -q -O - "https://api.github.com/repos/$REPO/releases/latest" |
      sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1
  fi
}

detect_target() {
  os=$(uname -s)
  arch=$(uname -m)
  case "$os" in
    Linux) os=linux ;;
    Darwin)
      os=macos
      # An x86_64 shell under Rosetta still runs on Apple Silicon.
      if [ "$arch" = x86_64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
        arch=arm64
      fi
      ;;
    *) die "unsupported operating system: $os (on Windows use install.ps1)" ;;
  esac
  case "$arch" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) die "unsupported architecture: $arch" ;;
  esac
  printf '%s-%s\n' "$os" "$arch"
}

sha256() {
  if has sha256sum; then
    sha256sum "$1" | cut -d ' ' -f 1
  elif has shasum; then
    shasum -a 256 "$1" | cut -d ' ' -f 1
  else
    return 1
  fi
}

add_to_path() { # bin_dir
  line="export PATH=\"$1:\$PATH\""
  marker="# Added by the adm installer"
  updated=""
  # ~/.profile serves sh/dash and bash login shells without ~/.bash_profile;
  # bash and zsh files are touched when present, or for zsh when it is the
  # user's shell (the macOS default).
  rcs="$HOME/.profile"
  [ ! -f "$HOME/.bashrc" ] || rcs="$rcs $HOME/.bashrc"
  [ ! -f "$HOME/.bash_profile" ] || rcs="$rcs $HOME/.bash_profile"
  case "${SHELL:-}" in */zsh) zsh_user=1 ;; *) zsh_user=0 ;; esac
  if [ -f "$HOME/.zshrc" ] || [ "$zsh_user" = 1 ]; then rcs="$rcs $HOME/.zshrc"; fi
  for rc in $rcs; do
    if [ -f "$rc" ] && grep -F "$marker" "$rc" >/dev/null 2>&1; then
      continue
    fi
    printf '\n%s\n%s\n' "$marker" "$line" >>"$rc"
    updated="$updated $rc"
  done
  fish_dir="${XDG_CONFIG_HOME:-$HOME/.config}/fish"
  if has fish || [ -d "$fish_dir" ]; then
    fish_rc="$fish_dir/conf.d/adm.fish"
    if [ ! -f "$fish_rc" ]; then
      mkdir -p "$fish_dir/conf.d"
      printf '%s\nfish_add_path -g "%s"\n' "$marker" "$1" >"$fish_rc"
      updated="$updated $fish_rc"
    fi
  fi
  [ -z "$updated" ] || say "added $1 to PATH in:$updated"
}

main() {
  version="${ADM_VERSION:-}"
  data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  install_dir="${ADM_INSTALL_DIR:-$data_home/adm}"
  bin_dir="${ADM_BIN_DIR:-$HOME/.local/bin}"
  modify_path=1
  [ "${ADM_NO_MODIFY_PATH:-0}" = 0 ] || modify_path=0

  while [ $# -gt 0 ]; do
    case "$1" in
      --version) [ $# -ge 2 ] || die "--version needs a value"; version="$2"; shift 2 ;;
      --dir) [ $# -ge 2 ] || die "--dir needs a value"; install_dir="$2"; shift 2 ;;
      --bin-dir) [ $# -ge 2 ] || die "--bin-dir needs a value"; bin_dir="$2"; shift 2 ;;
      --no-modify-path) modify_path=0; shift ;;
      -h | --help)
        echo "usage: install.sh [--version <tag>] [--dir <path>] [--bin-dir <path>] [--no-modify-path]"
        exit 0
        ;;
      *) die "unknown option: $1" ;;
    esac
  done

  has tar || die "tar is required"
  target=$(detect_target)
  if [ -z "$version" ]; then
    version=$(latest_version) || true
    [ -n "$version" ] || die "cannot determine the latest release; set ADM_VERSION"
  fi
  case "$version" in v*) ;; *) version="v$version" ;; esac

  base="${ADM_DOWNLOAD_BASE:-https://github.com/$REPO/releases/download/$version}"
  name="adm-$version-$target"
  archive="$name.tar.gz"

  tmp=$(mktemp -d 2>/dev/null || mktemp -d -t adm)
  trap 'rm -rf "$tmp"' EXIT INT TERM

  say "downloading adm $version ($target)"
  download "$base/$archive" "$tmp/$archive" || die "download failed: $base/$archive"

  if download "$base/SHA256SUMS.txt" "$tmp/SHA256SUMS.txt" 2>/dev/null; then
    expected=$(awk -v f="$archive" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS.txt")
    [ -n "$expected" ] || die "no checksum for $archive in SHA256SUMS.txt"
    actual=$(sha256 "$tmp/$archive") || die "sha256sum or shasum is required to verify the download"
    [ "$expected" = "$actual" ] || die "checksum mismatch for $archive"
    say "checksum verified"
  else
    say "warning: SHA256SUMS.txt not available, skipping checksum verification"
  fi

  mkdir -p "$tmp/extract"
  tar -xzf "$tmp/$archive" -C "$tmp/extract"
  [ -f "$tmp/extract/adm" ] || die "archive does not contain the adm binary"
  chmod +x "$tmp/extract/adm"

  # Replace the previous installation as a whole so stale files do not linger.
  mkdir -p "$(dirname "$install_dir")" "$bin_dir"
  rm -rf "$install_dir.new"
  mv "$tmp/extract" "$install_dir.new"
  rm -rf "$install_dir"
  mv "$install_dir.new" "$install_dir"
  ln -sf "$install_dir/adm" "$bin_dir/adm"

  say "installed $install_dir/adm"
  say "linked $bin_dir/adm"
  [ ! -f "$install_dir/adm.conf.example" ] ||
    say "example configuration: $install_dir/adm.conf.example (copy to ~/.adm.conf)"

  case ":${PATH:-}:" in
    *":$bin_dir:"*) ;;
    *)
      if [ "$modify_path" = 1 ]; then
        add_to_path "$bin_dir"
        say "restart your shell, or run: export PATH=\"$bin_dir:\$PATH\""
      else
        say "$bin_dir is not in PATH; add it to use adm"
      fi
      ;;
  esac
}

main "$@"
