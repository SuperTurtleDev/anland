# Maintainer: anland contributors
# SteamOS aarch64 xorg-xwayland, pinned to the installed vendor version.

pkgname=xorg-xwayland
pkgver=24.1.9
pkgrel=1.3
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
  "xwayland-${pkgver}.tar.xz::https://xorg.freedesktop.org/archive/individual/xserver/xwayland-${pkgver}.tar.xz"
  'xwayland.patch'
)
sha512sums=(
  '7438a572651dc77c1fd749879abccdc9a245c7b75143668d5561a8e99d41063f042a8eb3f9b931a2a12be1fc3cb9d197eee6794d0702a19e56c20f55acb35a26'
  'SKIP'
)
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
  patch -Np1 -d "$srcdir/xwayland-$pkgver" -i "$srcdir/xwayland.patch"
}

build() {
  local setup_args=()
  [[ ! -f "$srcdir/build/meson-private/coredata.dat" ]] || setup_args+=(--reconfigure)
  meson setup "${setup_args[@]}" "$srcdir/build" "$srcdir/xwayland-$pkgver" \
    --prefix=/usr \
    --libexecdir=lib \
    --sbindir=bin \
    --buildtype=plain \
    --auto-features=enabled \
    --wrap-mode=nodownload \
    -D b_pie=true \
    -D ipv6=true \
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
    "$srcdir/xwayland-$pkgver/COPYING"
}