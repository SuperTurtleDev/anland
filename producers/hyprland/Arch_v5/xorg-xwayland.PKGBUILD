# Maintainer: anland contributors
# Arch Linux ARM xorg-xwayland rebuilt with the anland kgsl/turnip fixes.

pkgname=xorg-xwayland
pkgver=0
pkgrel=1.1
pkgdesc='Run X clients under Wayland (anland kgsl/turnip fixes)'
arch=('aarch64')
license=(
  LicenseRef-Adobe-Display-PostScript
  BSD-3-Clause
  LicenseRef-DEC-3-Clause
  HPND
  LicenseRef-HPND-sell-MIT-disclaimer-xserver
  HPND-sell-variant
  ICU
  ISC
  MIT
  MIT-open-group
  NTP
  SGI-B-2.0
  SMLNJ
  X11
  X11-distribute-modifications-variant
)
groups=('xorg')
url='https://xorg.freedesktop.org'

depends=(
  nettle libepoxy libxfont2 pixman xorg-server-common libxcvt mesa libglvnd
  libxau wayland libdrm libtirpc libei libxshmfence libdecor glibc
)
makedepends=(
  meson ninja xorgproto xtrans libxkbfile dbus xorg-font-util
  wayland-protocols mesa-libgl systemd
)
source=(
  'xorg-xserver::git+https://gitlab.freedesktop.org/xorg/xserver.git'
  'xwayland.patch'
)
sha256sums=('SKIP' 'SKIP')

pkgver() {
  cd "$srcdir/xorg-xserver"
  local description
  description="$(git describe --long --tags --match 'xwayland-*' 2>/dev/null || true)"
  if [[ "$description" =~ ^xwayland-([0-9]+(\.[0-9]+)+)-([0-9]+)-g([0-9a-f]+)$ ]]; then
    printf '%s.r%s.g%s
' "${BASH_REMATCH[1]}" "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}"
  else
    printf '0.0.0.r%s.g%s
' "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
  fi
}
provides=('xorg-server-xwayland')
conflicts=('xorg-server-xwayland')
replaces=('xorg-server-xwayland')

require_aarch64() {
  [[ "$(uname -m)" == 'aarch64' && "${CARCH:-}" == 'aarch64' ]] || {
    error 'this package must be built natively on Arch Linux ARM (aarch64)'
    return 1
  }
}

prepare() {
  require_aarch64 || return 1
  patch --batch --fuzz=2 --no-backup-if-mismatch -Np1 -d "$srcdir/xorg-xserver" -i "$srcdir/xwayland.patch"
}

build() {
  meson setup "$srcdir/build" "$srcdir/xorg-xserver" \
    --prefix=/usr \
    --libexecdir=lib \
    --sbindir=bin \
    --buildtype=plain \
    --auto-features=enabled \
    --wrap-mode=nodownload \
    -D b_pie=true \
    -D python.bytecompile=1 \
    -D ipv6=true \
    -D xorg=false \
    -D xnest=false \
    -D xephyr=false \
    -D xvfb=false \
    -D xdmcp=false \
    -D xcsecurity=true \
    -D dri3=true \
    -D glamor=true \
    -D libdecor=true \
    -D xkb_dir=/usr/share/X11/xkb \
    -D xkb_output_dir=/var/lib/xkb
  meson configure "$srcdir/build"
  meson compile -C "$srcdir/build" -j "${JOBS:-$(nproc)}"
}

package() {
  DESTDIR="$pkgdir" meson install -C "$srcdir/build"

  # xorg-server-common owns these shared files.
  rm "$pkgdir/usr/lib/xorg/protocol.txt"
  rmdir "$pkgdir/usr/lib/xorg"
  rm "$pkgdir/usr/share/man/man1/Xserver.1"

  install -m644 -Dt "$pkgdir/usr/share/licenses/$pkgname" \
    "$srcdir/xorg-xserver/COPYING"
}
