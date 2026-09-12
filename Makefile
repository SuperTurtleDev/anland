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
#   make module     # SukiSU module zip (build/module/anland-awl.zip)
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

.PHONY: all check-tools native native-debug apk module libffi clean

all: native apk module
	md5sum "$(OUT)/waylandbridge" "$(OUT)/anland-wayland.apk" \
	       "$(OUT)/module/anland-awl.zip"

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
module: native
	MOD="$(OUT)/module/anland-awl"
	rm -rf "$(OUT)/module"
	mkdir -p "$$MOD"
	cp module/module.prop module/sepolicy.rule module/service.sh \
	   module/customize.sh module/plat_service_contexts.anland "$$MOD/"
	cp LICENSE LICENSE-COMMUNITY.txt LICENSE-CONTRIBUTOR.txt \
	   LICENSE-COMMERCIAL.txt "$$MOD/"
	cp "$(OUT)/waylandbridge" "$$MOD/"
	chmod 755 "$$MOD/waylandbridge" "$$MOD/service.sh" "$$MOD/customize.sh"
	(cd "$$MOD" && zip -qr "$(OUT)/module/anland-awl.zip" \
	  module.prop sepolicy.rule service.sh customize.sh \
	  plat_service_contexts.anland waylandbridge \
	  LICENSE LICENSE-COMMUNITY.txt LICENSE-CONTRIBUTOR.txt \
	  LICENSE-COMMERCIAL.txt)
	echo "OK: $(OUT)/module/anland-awl.zip"

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
