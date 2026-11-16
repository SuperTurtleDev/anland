package com.anland.consumer;

import android.net.LocalServerSocket;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;
import android.view.InputDevice;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;

/**
 * Root app_process child of inputgrab. Disables Android InputReader only; the
 * container keeps reading the original evdev node through its existing /dev/input mount.
 */
public final class GamepadInputLease {
    private static final String TAG = "AnlandGamepad";
    private static final int TIMEOUT_MS = 6000;

    private final Object input;
    private final Method getDevice, getDeviceIds, isEnabled, enable, disable;
    private final int deviceId;
    private final String descriptor;
    private final String devicePath;
    private final String deviceName;
    private final int vendorId, productId;
    private final LocalServerSocket owner;
    private boolean restore;
    private boolean closing;

    private GamepadInputLease(String[] args) throws Exception {
        if (args.length != 4)
            throw new IllegalArgumentException("expected path, name, vendor id and product id");
        devicePath = args[0];
        deviceName = args[1];
        if (devicePath == null || !devicePath.matches("/dev/input/event[0-9]+")
                || deviceName == null || deviceName.isEmpty()
                || !args[2].matches("[0-9a-fA-F]{4}")
                || !args[3].matches("[0-9a-fA-F]{4}"))
            throw new IllegalArgumentException("Invalid evdev controller identity");
        vendorId = Integer.parseInt(args[2], 16);
        productId = Integer.parseInt(args[3], 16);

        Class<?> manager = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) manager.getDeclaredMethod("getService", String.class)
                .invoke(null, "input");
        if (binder == null)
            throw new IllegalStateException("Input service unavailable");
        Class<?> stub = Class.forName("android.hardware.input.IInputManager$Stub");
        input = stub.getDeclaredMethod("asInterface", IBinder.class).invoke(null, binder);
        Class<?> api = Class.forName("android.hardware.input.IInputManager");
        getDevice = api.getMethod("getInputDevice", int.class);
        getDeviceIds = api.getMethod("getInputDeviceIds");
        isEnabled = api.getMethod("isInputDeviceEnabled", int.class);
        enable = api.getMethod("enableInputDevice", int.class);
        disable = api.getMethod("disableInputDevice", int.class);
        deviceId = findUniqueDeviceId();
        if (deviceId < 0)
            throw new IllegalStateException("No unique eligible Android controller for "
                    + devicePath + " '" + deviceName + "' (" + args[2] + ":" + args[3] + ")");
        InputDevice device = device();
        if (!GamepadDeviceIdentity.matches(deviceName, vendorId, productId, device))
            throw new IllegalStateException("Controller changed during identity lookup");
        descriptor = device.getDescriptor();
        if (descriptor == null || descriptor.isEmpty())
            throw new IllegalStateException("Controller descriptor unavailable");
        // An abstract socket serializes leases and is released even if this process dies.
        owner = new LocalServerSocket("anland.gamepad.lease." + deviceId);
    }

    private int findUniqueDeviceId() throws Exception {
        int[] ids = (int[]) getDeviceIds.invoke(input);
        if (ids == null)
            throw new IllegalStateException("Android input device list unavailable");
        GamepadDeviceIdentity.Candidate[] candidates = new GamepadDeviceIdentity.Candidate[ids.length];
        StringBuilder nearby = new StringBuilder();
        int identityCount = 0, eligibleCount = 0, virtualCount = 0, rejectedSources = 0;
        for (int i = 0; i < ids.length; i++) {
            InputDevice device = (InputDevice) getDevice.invoke(input, ids[i]);
            if (device == null)
                continue;
            String name = device.getName();
            int vendor = device.getVendorId();
            int product = device.getProductId();
            int sources = device.getSources();
            int keyboardType = device.getKeyboardType();
            boolean virtual = device.isVirtual();
            boolean identity = GamepadDeviceIdentity.sameIdentity(deviceName, vendorId,
                    productId, name, vendor, product);
            boolean eligible = GamepadDeviceIdentity.matches(deviceName, vendorId,
                    productId, name, vendor, product, sources, keyboardType);
            if (identity) {
                identityCount++;
                if (virtual)
                    virtualCount++;
                else if (eligible)
                    eligibleCount++;
                else
                    rejectedSources++;
            }
            if (name != null && name.equals(deviceName)
                    || vendor == vendorId && product == productId
                    || GamepadDeviceIdentity.hasControllerSource(sources)) {
                if (nearby.length() > 0)
                    nearby.append("; ");
                nearby.append("id=").append(ids[i])
                        .append(" name='").append(name).append('\'')
                        .append(" vid=").append(Integer.toHexString(vendor))
                        .append(" pid=").append(Integer.toHexString(product))
                        .append(" sources=0x").append(Integer.toHexString(sources))
                        .append(" keyboardType=").append(keyboardType)
                        .append(" virtual=").append(virtual)
                        .append(" identity=").append(identity)
                        .append(" eligible=").append(eligible);
            }
            if (!virtual)
                candidates[i] = new GamepadDeviceIdentity.Candidate(ids[i], name,
                        vendor, product, sources, keyboardType);
        }
        int match = GamepadDeviceIdentity.findUniqueDeviceId(deviceName, vendorId,
                productId, candidates);
        if (match < 0) {
            String cause = identityCount == 0 ? "identity-not-found"
                    : eligibleCount > 1 ? "ambiguous-identity"
                    : eligibleCount == 0 && virtualCount > 0 && rejectedSources == 0
                            ? "virtual-device-only"
                    : eligibleCount == 0 ? "non-controller-or-composite-sources"
                    : "same-identity-candidate-rejected";
            Log.e(TAG, "No unique controller match (" + cause + ") for " + devicePath
                    + " expectedName='" + deviceName + "' vid=" + Integer.toHexString(vendorId)
                    + " pid=" + Integer.toHexString(productId) + " identityCandidates="
                    + identityCount + " eligible=" + eligibleCount + " virtual="
                    + virtualCount + " rejectedSources=" + rejectedSources
                    + " nearby=[" + nearby + "]");
        }
        return match;
    }

    private InputDevice device() throws Exception {
        return (InputDevice) getDevice.invoke(input, deviceId);
    }

    private boolean sameIdentity(InputDevice device) {
        return device != null && !device.isVirtual() && descriptor.equals(device.getDescriptor())
                && GamepadDeviceIdentity.sameIdentity(deviceName, vendorId, productId,
                        device.getName(), device.getVendorId(), device.getProductId());
    }

    private boolean sameEnabledDevice(InputDevice device) {
        return sameIdentity(device)
                && GamepadDeviceIdentity.matches(deviceName, vendorId, productId, device);
    }

    private boolean sameDevice() throws Exception {
        return sameEnabledDevice(device());
    }

    private boolean enabled() throws Exception {
        return (Boolean) isEnabled.invoke(input, deviceId);
    }

    // Serialize suppression/restoration; shutdown must not overtake disable().
    private synchronized void acquire() throws Exception {
        if (closing)
            throw new IllegalStateException("Controller lease already closing");
        if (!sameDevice() || findUniqueDeviceId() != deviceId)
            throw new IllegalStateException("Controller changed before suppression");
        StructPollfd parent = new StructPollfd();
        parent.fd = FileDescriptor.in;
        parent.events = (short) OsConstants.POLLIN;
        Os.poll(new StructPollfd[]{parent}, 0);
        if ((parent.revents & (OsConstants.POLLHUP | OsConstants.POLLERR
                | OsConstants.POLLNVAL)) != 0)
            throw new IllegalStateException("Immersive helper already stopped");
        if (enabled()) {
            // Set before Binder: even a partially applied failing request needs restore.
            restore = true;
            disable.invoke(input, deviceId);
            waitEnabled(false);
        }
        if (!sameDevice() || findUniqueDeviceId() != deviceId)
            throw new IllegalStateException("Controller identity changed during suppression");
        if (enabled())
            throw new IllegalStateException("Android controller suppression was not retained");
        Log.i(TAG, "Android controller " + deviceId + " suppressed for " + devicePath
                + "; raw evdev stays in container");
    }

    private void waitEnabled(boolean target) throws Exception {
        long deadline = SystemClock.uptimeMillis() + 1500;
        // Restoration still applies if the same logical device changes its sources.
        while (sameIdentity(device()) && enabled() != target) {
            if (SystemClock.uptimeMillis() >= deadline)
                throw new IllegalStateException("InputReader state change timed out");
            Thread.sleep(20);
        }
    }

    private synchronized boolean release() {
        closing = true;
        if (!restore)
            return true;
        for (int attempt = 0; attempt < 3; attempt++) {
            try {
                if (sameIdentity(device())) {
                    enable.invoke(input, deviceId);
                    waitEnabled(true);
                }
                restore = false;
                return true;
            } catch (Exception error) {
                Log.e(TAG, "Could not restore Android controller " + deviceId, error);
            }
        }
        return false;
    }

    /** Called by app_process as root, never by an Activity in the app process. */
    public static void main(String[] args) {
        if (Process.myUid() != 0 || args.length != 4)
            return;
        GamepadInputLease lease = null;
        Thread cleanup = null;
        try (FileInputStream in = new FileInputStream(FileDescriptor.in);
             FileOutputStream out = new FileOutputStream(FileDescriptor.out)) {
            try {
                lease = new GamepadInputLease(args);
                final GamepadInputLease current = lease;
                cleanup = new Thread(() -> current.release(), "gamepad-restore");
                Runtime.getRuntime().addShutdownHook(cleanup);
                lease.acquire();
                out.write(1); // Confirm suppression before inputgrab announces PASSTHROUGH.
                out.flush();
                long lastBeat = SystemClock.uptimeMillis();
                StructPollfd fd = new StructPollfd();
                fd.fd = FileDescriptor.in;
                fd.events = (short) OsConstants.POLLIN;
                byte[] beat = new byte[64];
                String endReason;
                leaseLoop: for (;;) {
                    fd.revents = 0;
                    long silence = SystemClock.uptimeMillis() - lastBeat;
                    int waitMs = (int) Math.max(0L, Math.min(500L, TIMEOUT_MS - silence));
                    try {
                        Os.poll(new StructPollfd[]{fd}, waitMs);
                    } catch (ErrnoException error) {
                        if (error.errno == OsConstants.EINTR)
                            continue;
                        throw error;
                    }
                    if ((fd.revents & (OsConstants.POLLHUP | OsConstants.POLLERR
                            | OsConstants.POLLNVAL)) != 0) {
                        endReason = "helper socket closed";
                        break;
                    }
                    // Drain queued heartbeats before checking age after a delayed poll turn.
                    if ((fd.revents & OsConstants.POLLIN) != 0) {
                        int count = in.read(beat);
                        if (count < 0) {
                            endReason = "helper EOF";
                            break;
                        }
                        if (count > 0)
                            lastBeat = SystemClock.uptimeMillis();
                        for (int i = 0; i < count; i++) {
                            if (beat[i] == 'R') {
                                endReason = "helper released the session";
                                break leaseLoop;
                            }
                        }
                    }
                    silence = SystemClock.uptimeMillis() - lastBeat;
                    if (silence >= TIMEOUT_MS) {
                        endReason = "helper heartbeat timed out after " + silence + " ms";
                        break;
                    }
                    if (!lease.sameDevice()) {
                        endReason = "controller disconnected or replaced";
                        break;
                    }
                    if (lease.enabled()) {
                        endReason = "Android unexpectedly re-enabled the controller";
                        break;
                    }
                }
                Log.i(TAG, "Controller " + lease.deviceId + " lease ending: " + endReason);
            } finally {
                if (lease != null) {
                    if (!lease.release())
                        throw new IllegalStateException("Android controller restoration failed");
                    lease.owner.close();
                }
                if (cleanup != null)
                    Runtime.getRuntime().removeShutdownHook(cleanup);
            }
        } catch (Throwable error) {
            Log.e(TAG, "Controller passthrough unavailable: " + args[0], error);
        }
    }
}