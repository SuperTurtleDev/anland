# Makefile — awl repo-root build (build only; deployment lives in the workspace deploy-awl.sh, kept out of the repo)
#
# Environment:
#   ANDROID_HOME  SDK root (where cmdline-tools/sdkmanager lives; missing
#                 ndk/build-tools/platforms components are auto-installed
#                 via sdkmanager)
#   JAVA_HOME     JDK root (javac/keytool; its bin is prepended to PATH)
#
# Usage:
#   make            # everything: native + apk + module zip
#   make native     # single waylandbridge binary (logic+adapt layers linked in statically)
#                    #   Release: LOGD hot-path tracing compiled out (awl_log.h)
#   make native-debug  # same binary with LOGD tracing compiled in → build/waylandbridge-debug
#   make apk        # com.anlandnext APK (gradle project in app/, via gradlew)
#   make module     # SukiSU module zip (build/module/anland-awl.zip; includes pulse/)
#   make pulse      # PulseAudio for Android (Termux sink modules) → build/pulse-stage (module pulse/)
#   make pulse-deps # its static deps: libsndfile / libsoxr / libltdl (tarballs, sha256-pinned)
#   make anlandx    # in-container anland session (D-Bus + Xwayland + mini-wm), SOURCE tarball (build/anlandx.tar.gz)
#   make libffi     # one-time bootstrap: cross-compile libffi (skipped if present)
#   make clean
SHELL := /bin/bash
.ONESHELL:
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:

ANDROID_HOME ?=
JAVA_HOME    ?=
NDK_VERSION  ?= 29.0.13113456
BT_VERSION   ?= 36.0.0
PLATFORM_VER ?= android-36

ifneq ($(JAVA_HOME),)
PATH := $(JAVA_HOME)/bin:$(PATH)
export PATH
endif

SDKM     := $(ANDROID_HOME)/cmdline-tools/latest/bin/sdkmanager
NDK      := $(ANDROID_HOME)/ndk/$(NDK_VERSION)
BT       := $(ANDROID_HOME)/build-tools/$(BT_VERSION)
PLATFORM := $(ANDROID_HOME)/platforms/$(PLATFORM_VER)/android.jar
JAVA     := $(JAVA_HOME)/bin
BUILD    := build/arm64
BUILD_DBG:= build/arm64-debug
OUT      := $(abspath build)

# ---- PulseAudio for Android (module pulse/) ----
# PA_PREFIX is baked into the binaries (module dir: config, modlibexecdir).
# No trailing comments on these lines: make keeps the blanks before a '#'.
PA_PREFIX := /data/adb/modules/anland-awl/pulse
PA_API    := 35
PA_TC     := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
PA_DEPS   := $(OUT)/pulse-deps
PA_STAGE  := $(OUT)/pulse-stage
PA_ROOT   := $(PA_STAGE)$(PA_PREFIX)

.PHONY: all check-tools native native-debug apk module pulse pulse-deps anlandx libffi clean

all: native apk module anlandx
	md5sum "$(OUT)/waylandbridge" "$(OUT)/anland-wayland.apk" \
	       "$(OUT)/module/anland-awl.zip" "$(OUT)/anlandx.tar.gz"

# ---------------- Toolchain self-check (missing components → sdkmanager install) ----------------
check-tools:
	[ -n "$(ANDROID_HOME)" ] || { echo "ERROR: ANDROID_HOME not set (SDK root directory)"; exit 1; }
	[ -n "$(JAVA_HOME)" ] || { echo "ERROR: JAVA_HOME not set (JDK root directory)"; exit 1; }
	[ -x "$(SDKM)" ] || { echo "ERROR: sdkmanager missing: $(SDKM) (need cmdline-tools;latest)"; exit 1; }
	MISS=()
	[ -d "$(NDK)" ] || MISS+=("ndk;$(NDK_VERSION)")
	[ -d "$(ANDROID_HOME)/build-tools/$(BT_VERSION)" ] || MISS+=("build-tools;$(BT_VERSION)")
	[ -f "$(PLATFORM)" ] || MISS+=("platforms;$(PLATFORM_VER)")
	if [ "$${#MISS[@]}" -gt 0 ]; then
	  echo "sdkmanager installing missing components: $${MISS[*]}"
	  yes | "$(SDKM)" --licenses >/dev/null 2>&1 || true
	  "$(SDKM)" "$${MISS[@]}" >/dev/null
	fi

# ---------------- native (cmake: services/waylandbridge) ----------------
native: check-tools
	cmake -S services/waylandbridge -B "$(BUILD)" \
	  -DCMAKE_TOOLCHAIN_FILE="$(NDK)/build/cmake/android.toolchain.cmake" \
	  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
	  -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "$(BUILD)" -j$$(nproc)
	mkdir -p "$(OUT)"
	cp -f "$(BUILD)/out/waylandbridge" "$(OUT)/"
	cp -f "$(BUILD)/out/libawlshm.so" "$(OUT)/"
	ls -la "$(OUT)/waylandbridge" "$(OUT)/libawlshm.so"

# ---------------- native with LOGD hot-path tracing (development) ----------------
# Separate build dir so switching does not thrash the release cache; output is
# build/waylandbridge-debug (never shipped in the module zip).
native-debug: check-tools
	cmake -S services/waylandbridge -B "$(BUILD_DBG)" \
	  -DCMAKE_TOOLCHAIN_FILE="$(NDK)/build/cmake/android.toolchain.cmake" \
	  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
	  -DCMAKE_BUILD_TYPE=Debug -DAWL_LOG_DEBUG=ON >/dev/null
	cmake --build "$(BUILD_DBG)" -j$$(nproc)
	cp -f "$(BUILD_DBG)/out/waylandbridge" "$(OUT)/waylandbridge-debug"
	ls -la "$(OUT)/waylandbridge-debug"

# ---------------- APK + third-party AAR (gradle: app/ → AGP, artifacts collected into build/) ----------------
# Pure-Java APK: rendering/protocol all live in the root daemon — no
# dependencies, no native libs. Signed with app/debug.keystore (gitignored;
# bootstrapped here with keytool when missing).
# :libawl = third-party client library (wayland fd / window list / events /
# window hosting) — published as build/anland-awllib.aar.
apk: check-tools
	KS=app/debug.keystore
	if [ ! -f "$$KS" ]; then
	  "$(JAVA)/keytool" -genkeypair -keystore "$$KS" -alias anland \
	    -storepass anland -keypass anland -keyalg RSA -keysize 2048 \
	    -validity 10000 -dname "CN=Anland Debug" >/dev/null 2>&1
	fi
	(cd app && ./gradlew --no-daemon -q assembleRelease :libawl:assembleRelease)
	cp -f app/build/outputs/apk/release/anland-wayland-release.apk "$(OUT)/anland-wayland.apk"
	cp -f app/libawl/build/outputs/aar/libawl-release.aar "$(OUT)/anland-awllib.aar"
	"$(BT)/apksigner" verify --print-certs "$(OUT)/anland-wayland.apk" | head -3
	ls -la "$(OUT)/anland-wayland.apk" "$(OUT)/anland-awllib.aar"
	echo "OK: $(OUT)/anland-wayland.apk + $(OUT)/anland-awllib.aar"

# ---------------- Test app APK (third-party-path harness, #36) ----------------
# Packages libawlshm.so (built by `make native`) into :testapp's jniLibs and
# builds com.anlandtest — a separate uid exercising the full third-party
# path: binder wayland fd (fork-inherited WAYLAND_SOCKET), own-uid window
# list/events, self-hosted windows via libawl.
testapk: native
	mkdir -p app/testapp/src/main/jniLibs/arm64-v8a
	cp -f "$(OUT)/libawlshm.so" app/testapp/src/main/jniLibs/arm64-v8a/libawlshm.so
	(cd app && ./gradlew --no-daemon -q :testapp:assembleRelease)
	cp -f app/testapp/build/outputs/apk/release/testapp-release.apk "$(OUT)/anland-testapp.apk"
	"$(BT)/apksigner" verify --print-certs "$(OUT)/anland-testapp.apk" | head -3
	ls -la "$(OUT)/anland-testapp.apk"
	echo "OK: $(OUT)/anland-testapp.apk"

# ---------------- SukiSU module zip ----------------
# Template lives in module/ (module.prop / sepolicy.rule / service.sh /
# customize.sh / the plat_service_contexts.anland fragment). The full
# contexts file is NOT zipped — customize.sh generates it at flash time from
# the device's live system original (guards against stale OTA shadowing);
# update installs are idempotent. The build only merges in the latest
# waylandbridge binary.
module: native pulse
	MOD="$(OUT)/module/anland-awl"
	rm -rf "$(OUT)/module"
	mkdir -p "$$MOD"
	cp module/module.prop module/sepolicy.rule module/service.sh \
	   module/customize.sh module/post-fs-data.sh module/service-contexts.sh \
	   module/plat_service_contexts.anland "$$MOD/"
	cp LICENSE "$$MOD/"
	cp "$(OUT)/waylandbridge" "$$MOD/"
	cp -r "$(PA_ROOT)" "$$MOD/pulse"   # PulseAudio tree (bin/lib/etc), see `make pulse`
	chmod 755 "$$MOD/waylandbridge" "$$MOD/service.sh" "$$MOD/customize.sh" "$$MOD/post-fs-data.sh" "$$MOD"/pulse/bin/*
	(cd "$$MOD" && zip -qr "$(OUT)/module/anland-awl.zip" \
	  module.prop sepolicy.rule service.sh customize.sh post-fs-data.sh service-contexts.sh \
	  plat_service_contexts.anland waylandbridge pulse \
	  LICENSE)
	echo "OK: $(OUT)/module/anland-awl.zip"

# ---------------- PulseAudio for Android ----------------
# Termux's pulseaudio (termux-packages packages/pulseaudio: bionic patches +
# module-sles-sink / module-aaudio-sink, vendored in pulse/termux/) built
# with our NDK against pulseaudio v17.0 (submodule third_party/pulseaudio).
# Playback only. Installed at PA_PREFIX inside the module; service.sh runs it
# as root in the awl_daemon domain, socket <runtime_dir>/pulse.sock — the
# container's libpulse clients connect through /run/anland/pulse.sock
# (setupanlandx.sh writes the client.conf). Host tools: meson ninja cmake patch.
# adrian-aec=true only satisfies meson's "one echo canceller" sanity check
# with the dependency-free built-in (Termux pulls libwebrtc-audio-processing
# for it); module-echo-cancel is never loaded here.
pulse-deps: check-tools
	if [ -f "$(PA_DEPS)/lib/libsndfile.a" ] && [ -f "$(PA_DEPS)/lib/libsoxr.a" ] && \
	   [ -f "$(PA_DEPS)/lib/libltdl.a" ]; then echo "pulse deps already built"; exit 0; fi
	command -v cmake >/dev/null || { echo "ERROR: cmake missing"; exit 1; }
	DL=build/pulse-dl; mkdir -p "$$DL"
	fetch() {   # fetch <file> <url> <sha256>
	  if [ ! -f "$$DL/$$1" ]; then curl -fsSL --retry 3 -o "$$DL/$$1" "$$2"; fi
	  echo "$$3  $$DL/$$1" | sha256sum -c --quiet || { echo "ERROR: checksum $$1"; rm -f "$$DL/$$1"; exit 1; }
	}
	fetch libtool-2.4.7.tar.xz https://ftpmirror.gnu.org/libtool/libtool-2.4.7.tar.xz \
	  4f7f217f057ce655ff22559ad221a0fd8ef84ad1fc5fcb6990cecc333aa1635d
	fetch libsndfile-1.2.2.tar.xz https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz \
	  3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e
	fetch soxr-0.1.3.tar.gz https://github.com/chirlu/soxr/archive/refs/tags/0.1.3.tar.gz \
	  db6ca1b1e8405c6ef92f8294fc123d910abf0a114003b3f0f13fa57a95fd62d0
	rm -rf build/pulse-deps-src "$(PA_DEPS)"; mkdir -p build/pulse-deps-src "$(PA_DEPS)"
	for t in "$$DL"/*.tar.*; do tar -C build/pulse-deps-src -xf "$$t"; done
	# static + PIC: they end up inside libpulsecommon.so; CMAKE_POLICY_VERSION_MINIMUM
	# keeps cmake ≥ 4 accepting their old cmake_minimum_required
	CM="-DCMAKE_TOOLCHAIN_FILE=$(NDK)/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
	    -DANDROID_PLATFORM=android-$(PA_API) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PA_DEPS) \
	    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
	cmake -S build/pulse-deps-src/libsndfile-1.2.2 -B build/pulse-deps-src/sndfile-build $$CM \
	  -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF \
	  -DBUILD_TESTING=OFF -DBUILD_REGTEST=OFF -DENABLE_CPACK=OFF -DENABLE_PACKAGE_CONFIG=OFF >/dev/null
	cmake --build build/pulse-deps-src/sndfile-build -j$$(nproc) >/dev/null
	cmake --install build/pulse-deps-src/sndfile-build >/dev/null
	cmake -S build/pulse-deps-src/soxr-0.1.3 -B build/pulse-deps-src/soxr-build $$CM \
	  -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF -DWITH_LSR_BINDINGS=OFF >/dev/null
	cmake --build build/pulse-deps-src/soxr-build -j$$(nproc) >/dev/null
	cmake --install build/pulse-deps-src/soxr-build >/dev/null
	cd build/pulse-deps-src/libtool-2.4.7/libltdl
	CC="$(PA_TC)/aarch64-linux-android$(PA_API)-clang" AR="$(PA_TC)/llvm-ar" RANLIB="$(PA_TC)/llvm-ranlib" \
	CFLAGS="-O2 -fPIC" ./configure --host=aarch64-linux-android --prefix="$(PA_DEPS)" \
	  --enable-static --disable-shared --enable-ltdl-install >/dev/null
	make -j$$(nproc) >/dev/null && make install >/dev/null
	ls -la "$(PA_DEPS)"/lib/*.a
	echo "OK: pulse deps in $(PA_DEPS)"

pulse: pulse-deps
	command -v meson >/dev/null && command -v ninja >/dev/null && command -v patch >/dev/null \
	  || { echo "ERROR: need meson, ninja, patch on the build host"; exit 1; }
	git submodule update --init third_party/pulseaudio
	bash pulse/prepare-src.sh build/pulse-src
	sed -e "s|@TC@|$(PA_TC)|g;s|@API@|$(PA_API)|g;s|@DEPS@|$(PA_DEPS)|g" \
	  pulse/meson-cross.ini.in > build/pulse-cross.ini
	rm -rf build/pulse-build
	meson setup build/pulse-build build/pulse-src --cross-file build/pulse-cross.ini \
	  --buildtype=release --strip \
	  --prefix="$(PA_PREFIX)" --libdir=lib --sysconfdir=etc --localstatedir=var \
	  -Ddaemon=true -Dclient=true -Ddoxygen=false -Dman=false -Dtests=false \
	  -Ddatabase=simple -Dsoxr=enabled -Dipv6=false \
	  -Dalsa=disabled -Dasyncns=disabled -Davahi=disabled -Dbluez5=disabled \
	  -Dconsolekit=disabled -Ddbus=disabled -Delogind=disabled -Dfftw=disabled \
	  -Dglib=disabled -Dgsettings=disabled -Dgstreamer=disabled -Dgtk=disabled \
	  -Dhal-compat=false -Djack=disabled -Dlirc=disabled -Dopenssl=disabled \
	  -Dorc=disabled -Doss-output=disabled -Dsamplerate=disabled -Dspeex=disabled \
	  -Dsystemd=disabled -Dtcpwrap=disabled -Dudev=disabled -Dvalgrind=disabled \
	  -Dx11=disabled -Dadrian-aec=true -Dwebrtc-aec=disabled \
	  -Dbashcompletiondir=no -Dzshcompletiondir=no > build/pulse-meson-setup.log
	meson compile -C build/pulse-build
	rm -rf "$(PA_STAGE)"
	DESTDIR="$(PA_STAGE)" meson install -C build/pulse-build --quiet
	install -m 644 pulse/default.pa "$(PA_ROOT)/etc/pulse/default.pa"
	rm -rf "$(PA_ROOT)/include" "$(PA_ROOT)/lib/pkgconfig" "$(PA_ROOT)/lib/cmake" "$(PA_ROOT)/share"   # dev/doc files: not shipped
	du -sh "$(PA_ROOT)"
	echo "OK: $(PA_ROOT) (bin/pulseaudio + lib + etc/pulse/default.pa)"

# ---------------- anlandx: in-container anland session (source tarball) ----------------
# Runs INSIDE the Linux container (Ubuntu arm64) as a systemd --user service:
# anland-session sets up the session D-Bus ($XDG_RUNTIME_DIR/bus — system
# user bus, else dbus-launch'd), links the anland wayland socket into
# /run/user/<uid> as wayland-anland and publishes the app environment in
# ~/.anlandx-env; Xwayland -rootless picks a free display (-displayfd),
# publishes ":N" in ~/.anlandx, mini-wm surfaces the X windows and serves
# the daemon's resize/close channel on <runtime_dir>/anland-wm.sock
# (container view: /run/anland = ANLAND_RUNTIME_DIR convention). Shipped as
# SOURCE — the container has gcc + libx11-dev + libxcomposite-dev, so
# setupanlandx.sh compiles anland-session/miniwm.c on the device (no cross
# toolchain, no SDK needed here).
# Also ships two patched components — same scheme as pulse/termux/: the
# submodules stay pristine, the anland delta lives in patches/<component>/
# and is applied to the staged copy here. deps.sh (run first by
# setupanlandx.sh) installs the build deps on Debian/Ubuntu, incl.
# `apt-get build-dep bubblewrap xwayland` — the distro's own dep lists, so
# the -dev versions match whatever the distro ships. No distro source code
# is ever fetched; the system bwrap/xwayland packages are never touched.
#   bubblewrap/  third_party/bubblewrap v0.11.1 + patches/bubblewrap: the
#                mountinfo index fix (16 GB id-indexed table on KernelSU+
#                SuSFS → sorted array + bsearch; see the README there).
#                setupanlandx.sh compiles it into ~/.local/bin/bwrap
#                unconditionally — glycin (GTK SVG loading) then survives
#                on SuSFS devices too (VS Code died on its file chooser).
#   xserver/     third_party/xserver pinned to xwayland-24.1 + patches/xwayland:
#                the kgsl/turnip glamor fixes. setupanlandx.sh builds
#                Xwayland with meson into ~/.local/bin/Xwayland; ANLAND-SOURCE
#                records commit + patch checksum so unchanged builds are
#                skipped on re-run.
anlandx:
	command -v patch >/dev/null || { echo "ERROR: need patch on the build host"; exit 1; }
	[ -f third_party/bubblewrap/bubblewrap.c ] || { echo "ERROR: third_party/bubblewrap not checked out — git submodule update --init third_party/bubblewrap"; exit 1; }
	[ -f third_party/xserver/hw/xwayland/meson.build ] || { echo "ERROR: third_party/xserver not checked out — git submodule update --init third_party/xserver"; exit 1; }
	rm -rf "$(OUT)/anlandx"
	mkdir -p "$(OUT)/anlandx/bubblewrap" "$(OUT)/anlandx/xserver"
	cp anland-session/miniwm.c anland-session/anland-session.sh \
	   anland-session/anland-session.service anland-session/deps_noble.sh \
	   LICENSE "$(OUT)/anlandx/"
	cp anland-session/setup.sh "$(OUT)/anlandx/setupanlandx.sh"
	cp -r patches "$(OUT)/anlandx/patches"
	# bubblewrap: pristine v0.11.1 sources + anland config.h, patch applied below
	cp third_party/bubblewrap/*.c third_party/bubblewrap/*.h \
	   third_party/bubblewrap/COPYING "$(OUT)/anlandx/bubblewrap/"
	cp patches/bubblewrap/config.h "$(OUT)/anlandx/bubblewrap/"
	for p in patches/bubblewrap/*.patch; do
	  patch -d "$(OUT)/anlandx/bubblewrap" -p1 -s --no-backup-if-mismatch < "$$p"
	done
	# xserver: tracked files of the pinned xwayland-24.1 submodule, patched the same way
	git -C third_party/xserver archive --format=tar HEAD | tar -xf - -C "$(OUT)/anlandx/xserver"
	for p in patches/xwayland/*.patch; do
	  patch -d "$(OUT)/anlandx/xserver" -p1 -s --no-backup-if-mismatch < "$$p"
	done
	# xorgproto (release tarball, sha256-pinned — the pulse-deps pattern) as the
	# xserver meson subproject fallback: the proto deps (presentproto >= 1.4 …)
	# need it wherever the distro's xorgproto is older (Ubuntu 24.04 ships 1.3)
	DL="$(OUT)/dl"; mkdir -p "$$DL"
	if [ ! -f "$$DL/xorgproto-2024.1.tar.xz" ]; then
	  curl -fsSL --retry 3 -o "$$DL/xorgproto-2024.1.tar.xz" \
	    https://xorg.freedesktop.org/releases/individual/proto/xorgproto-2024.1.tar.xz
	fi
	echo "372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59  $$DL/xorgproto-2024.1.tar.xz" \
	  | sha256sum -c --quiet || { echo "ERROR: checksum xorgproto-2024.1.tar.xz"; exit 1; }
	rm -rf "$(OUT)/anlandx/xserver/subprojects"
	mkdir -p "$(OUT)/anlandx/xserver/subprojects"
	tar -C "$(OUT)/anlandx/xserver/subprojects" -xJf "$$DL/xorgproto-2024.1.tar.xz"
	mv "$(OUT)/anlandx/xserver/subprojects/xorgproto-2024.1" \
	   "$(OUT)/anlandx/xserver/subprojects/xorgproto"
	# the exact input the patched tree was built from — setupanlandx.sh skips
	# rebuilding ~/.local/bin/Xwayland when this matches its stamp
	{ git -C third_party/xserver rev-parse HEAD; cat patches/xwayland/*.patch | md5sum; } \
	  > "$(OUT)/anlandx/xserver/ANLAND-SOURCE"
	chmod 755 "$(OUT)/anlandx/setupanlandx.sh" "$(OUT)/anlandx/anland-session.sh" \
	   "$(OUT)/anlandx/deps_noble.sh"
	tar -C "$(OUT)" --owner=0 --group=0 -czf "$(OUT)/anlandx.tar.gz" anlandx
	ls -la "$(OUT)/anlandx.tar.gz"
	echo "OK: $(OUT)/anlandx.tar.gz  (device: tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh)"

# ---------------- One-time bootstrap: cross-compile libffi ----------------
# Source = git submodule third_party/libffi (pinned at v3.4.6);
# output build/libffi-android-out (build artifacts stay out of third_party).
libffi: check-tools
	if [ -f build/libffi-android-out/lib/libffi.a ]; then
	  echo "libffi already built"; exit 0
	fi
	git submodule update --init third_party/libffi
	if [ ! -x third_party/libffi/configure ]; then
	  (cd third_party/libffi && ./autogen.sh)   # git checkouts ship no configure (release tarballs do)
	fi
	TC="$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin"
	rm -rf build/libffi-android
	mkdir -p build/libffi-android
	cd build/libffi-android
	CC="$$TC/aarch64-linux-android35-clang" \
	CXX="$$TC/aarch64-linux-android35-clang++" \
	AR="$$TC/llvm-ar" RANLIB="$$TC/llvm-ranlib" STRIP="$$TC/llvm-strip" \
	  ../../third_party/libffi/configure --host=aarch64-linux-android \
	    --prefix="$$PWD/../libffi-android-out" \
	    --enable-static --disable-shared --disable-docs \
	    --with-sysroot="$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
	make -j$$(nproc)
	make install
	echo "libffi installed to build/libffi-android-out"

clean:
	rm -rf "$(BUILD)" "$(BUILD_DBG)" app/build "$(OUT)/module"
