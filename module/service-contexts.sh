# Shared by customize.sh and the pre-mount OTA guard.
# Exclude only Anland's own mapping so reinstalling while our overlay is
# visible produces the same fingerprint as the unobscured platform file.
anland_context_fingerprint() {
  [ -r "$1" ] && [ -s "$1" ] || return 1
  fingerprint=$(awk '$1 != "anland.host"' "$1" | sha256sum | awk '{print $1}')
  case "$fingerprint" in
    ''|*[!0-9a-f]*) return 1 ;;
  esac
  [ "${#fingerprint}" -eq 64 ] || return 1
  printf '%s\n' "$fingerprint"
}
