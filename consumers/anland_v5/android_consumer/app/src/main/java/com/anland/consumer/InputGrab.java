package com.anland.consumer;

import android.content.Context;
import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.TreeSet;
import java.util.regex.Pattern;

/**
 * Transport for immersive mode: runs the bundled {@code libinputgrab.so} helper as
 * root and turns its record stream into callbacks on the main thread.
 *
 * The app cannot open {@code /dev/input} itself (untrusted_app is denied
 * {@code input_device:chr_file}), so the helper does it in the root context and
 * streams the raw evdev events back. This is the same bridge shape
 * {@code fd_helper.c} uses for the daemon connection, except the app is the one
 * listening and the payload is events rather than a file descriptor.
 *
 * Because the helper holds an exclusive grab on the touchscreen while it runs,
 * every failure mode here has to end with that grab released:
 * <ul>
 *   <li>closing the socket makes the helper see EOF and ungrab — and the kernel
 *       closes it for us even if this process is killed outright;</li>
 *   <li>the heartbeat below is written only while the <em>main thread</em> is
 *       still running, so an ANR — not just a crash — also frees the input;</li>
 *   <li>the helper watches the toggle key itself, which is what rescues a
 *       session whose app went away without either of the above working;</li>
 *   <li>and if the helper is somehow still alive shortly after {@link #stop}, it
 *       is killed by the token this session put in its argv.</li>
 * </ul>
 */
final class InputGrab implements InputGrabTransport {
    private static final String TAG = "AnlandGrab";

    /** The direct transport: grabs physical nodes through the root helper. */
    static final InputGrabTransport.Factory FACTORY = InputGrab::new;

    /** Wire record size; see jni/input_grab.h. */
    private static final int REC_SIZE = 32;
    /** Byte offset of aux[0] inside a record. */
    private static final int AUX0 = 12;

    private static final int PROTO_VERSION = 3;

    // rtype
    private static final int REC_HELLO  = 1;
    private static final int REC_DEVICE = 2;
    private static final int REC_READY  = 3;
    private static final int REC_EVENT  = 4;
    private static final int REC_BYE    = 5;
    private static final int REC_AXIS_RANGE = 6;

    // DEVICE.etype (device class)
    static final int CLASS_TOUCHSCREEN = 1;
    static final int CLASS_TOUCHPAD    = 2;
    static final int CLASS_MOUSE       = 3;
    static final int CLASS_KEYBOARD    = 4;
    static final int CLASS_GAMEPAD     = 5;

    // DEVICE.aux[4] flags
    static final int DEV_GRABBED    = 1;
    static final int DEV_CLICKPAD   = 4;
    static final int DEV_PASSTHROUGH = 8;

    /** Broadcast device id used by the helper's "records were dropped" marker. */
    static final int DEV_ALL = 0xFFFF;

    // Session end reasons. 1..4 and 8 mirror IGRAB_BYE_*; 5..7 are local.
    static final int REASON_TOGGLE     = 1;  // toggle key pressed on a grabbed device
    static final int REASON_PEER_GONE  = 2;
    static final int REASON_STALLED    = 3;
    static final int REASON_HELPER_ERR = 4;  // helper found nothing it could grab
    static final int REASON_NO_ROOT    = 5;  // helper never connected (su denied/missing)
    static final int REASON_IO         = 6;  // stream broke
    static final int REASON_STOPPED    = 7;  // stop() was called
    static final int REASON_CONTROLLER = 8;  // selected controller suppression failed/lost

    /** How long to wait for the helper to connect; a first-time su prompt is slow. */
    private static final int CONNECT_TIMEOUT_MS = 20000;
    private static final int HEARTBEAT_INTERVAL_MS = 1000;
    /** The main thread must have run this recently for a heartbeat to be sent. */
    private static final int MAIN_ALIVE_WINDOW_MS = 2500;
    private static final int MAIN_BEACON_INTERVAL_MS = 500;
    /** Grace period for the helper to notice a closed socket before we kill it. */
    private static final int KILL_FALLBACK_MS = 1500;

    // ---- node-list arguments ---------------------------------------------

    /**
     * Legacy grammar, used for {@code nodes=}: matches whatever a saved
     * preference may legitimately contain, {@code event01} included.
     */
    private static final Pattern LEGACY_NODE = Pattern.compile("event[0-9]+");
    /** Strict grammar, used for {@code exclude=}: only what Gold itself will name. */
    private static final Pattern STRICT_NODE = Pattern.compile("event(?:0|[1-9][0-9]*)");
    /** Mirrors IGRAB_MAX_DEVICES in input_grab.h. */
    static final int MAX_SELECTED_NODES = 32;

    /**
     * Builds the {@code nodes=} argument from a saved selection.
     *
     * @return {@code ""} when there is nothing to send, or {@code null} when the
     *         list cannot be honoured and the caller must not start a session.
     */
    static String serializeSelectedNodes(Collection<String> nodes) {
        return serializeNodes("nodes=", nodes, LEGACY_NODE, false);
    }

    /**
     * Builds the {@code exclude=} argument. Exclusions are the one place a wrong
     * node silently takes input away from Gold, so this grammar is strict and a
     * duplicated entry is refused rather than collapsed.
     *
     * @return {@code ""} when there is nothing to exclude, or {@code null} when
     *         the list is not one we are willing to act on.
     */
    static String serializeExcludedNodes(Collection<String> nodes) {
        return serializeNodes("exclude=", nodes, STRICT_NODE, true);
    }

    private static String serializeNodes(String prefix, Collection<String> nodes, Pattern grammar,
                                         boolean rejectDuplicates) {
        if (nodes == null || nodes.isEmpty())
            return "";
        TreeSet<String> sorted = new TreeSet<>();
        for (String node : nodes) {
            if (node == null || !grammar.matcher(node).matches())
                return null;
            sorted.add(node);
        }
        if (rejectDuplicates && sorted.size() != nodes.size())
            return null;
        if (sorted.size() > MAX_SELECTED_NODES)
            return null;
        StringBuilder builder = new StringBuilder(prefix);
        for (String node : sorted) {
            if (builder.length() > prefix.length())
                builder.append(',');
            builder.append(node);
        }
        return builder.toString();
    }

    // ---- discovery --------------------------------------------------------

    /** How long the enumeration helper gets before it is treated as wedged. */
    private static final long DISCOVERY_TIMEOUT_MS = 5000L;

    /** One input node as the root helper sees it. */
    static final class DiscoveredDevice {
        /** Node name, e.g. {@code event12}. */
        final String node;
        /** EVIOCGNAME; empty when the kernel would not say. */
        final String name;
        /** One of the CLASS_* constants. */
        final int cls;
        /** Carries a full set of letter keys: a keyboard, not a button node. */
        final boolean alphaKeys;
        /** Classified watch-only, so a session never grabs it. */
        final boolean watchOnly;
        /** Bus type from EVIOCGID: how the device is attached. */
        final int bus;

        DiscoveredDevice(String node, String name, int cls, int bus, boolean alphaKeys,
                         boolean watchOnly) {
            this.node = node;
            this.name = name;
            this.cls = cls;
            this.bus = bus;
            this.alphaKeys = alphaKeys;
            this.watchOnly = watchOnly;
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof DiscoveredDevice))
                return false;
            DiscoveredDevice that = (DiscoveredDevice) other;
            return cls == that.cls && bus == that.bus && alphaKeys == that.alphaKeys
                    && watchOnly == that.watchOnly
                    && node.equals(that.node) && name.equals(that.name);
        }

        @Override
        public int hashCode() {
            int result = node.hashCode() * 31 + name.hashCode();
            result = result * 31 + cls;
            result = result * 31 + bus;
            result = result * 31 + (alphaKeys ? 1 : 0);
            return result * 31 + (watchOnly ? 2 : 0);
        }

        @Override
        public String toString() {
            return node + " " + name + " (" + cls + ",bus" + Integer.toHexString(bus)
                    + (alphaKeys ? ",alpha" : "") + (watchOnly ? ",watch" : "") + ")";
        }
    }

    // Bus types from linux/input.h, for describing how a device is attached.
    static final int BUS_USB = 0x03;
    static final int BUS_BLUETOOTH = 0x05;
    static final int BUS_VIRTUAL = 0x06;
    static final int BUS_I8042 = 0x11;
    static final int BUS_I2C = 0x18;
    static final int BUS_HOST = 0x19;
    static final int BUS_SPI = 0x1c;

    /** Real input devices remain visible even when Android must keep them. */
    static boolean isVisible(DiscoveredDevice device) {
        // Dedicated power/volume nodes and consumer-control button clusters are
        // still omitted; a protected touchscreen is a real device, not a cluster.
        return device.cls != CLASS_KEYBOARD || device.alphaKeys;
    }

    static boolean isSelectable(DiscoveredDevice device) {
        return isVisible(device) && !device.watchOnly;
    }

    /**
     * Node order as a user reads it: event0, event1, … event12.
     *
     * <p>The helper reports them in whatever order {@code readdir} returns, which
     * on a real device is neither sorted nor stable, and a list that reshuffles
     * itself between visits is one nobody can use.
     */
    private static final Comparator<DiscoveredDevice> BY_NODE_NUMBER =
            new Comparator<DiscoveredDevice>() {
                @Override
                public int compare(DiscoveredDevice left, DiscoveredDevice right) {
                    int a = nodeNumber(left.node);
                    int b = nodeNumber(right.node);
                    if (a != b)
                        return a < b ? -1 : 1;
                    return left.node.compareTo(right.node);
                }
            };

    private static int nodeNumber(String node) {
        try {
            return Integer.parseInt(node.substring("event".length()));
        } catch (RuntimeException e) {
            // Validated by DISCOVERED_NODE before we get here; this only guards
            // against a number too long for an int.
            return Integer.MAX_VALUE;
        }
    }

    /** Node spelling enumeration may produce: the grammar saved preferences use. */
    private static final Pattern DISCOVERED_NODE = Pattern.compile("event[0-9]+");

    /**
     * Parses the helper's {@code --list} output: one tab separated
     * {@code node, name, class} line per input node. Anything malformed is
     * dropped rather than guessed at.
     *
     * <p>Android-free so the discovery page's input can be tested on the JVM.
     */
    static List<DiscoveredDevice> parseDeviceList(String stdout) {
        List<DiscoveredDevice> devices = new ArrayList<>();
        if (stdout == null)
            return devices;
        for (String raw : stdout.split("\n")) {
            String line = raw.trim();
            if (line.isEmpty())
                continue;
            String[] fields = line.split("\t", -1);
            // node, name, class, bus, flags — the helper ships inside this APK,
            // so there is no older shape to stay compatible with.
            if (fields.length != 5)
                continue;
            if (!DISCOVERED_NODE.matcher(fields[0]).matches())
                continue;
            int cls;
            int bus;
            try {
                cls = Integer.parseInt(fields[2].trim());
                bus = Integer.parseInt(fields[3].trim(), 16);
            } catch (NumberFormatException e) {
                continue;
            }
            if (cls < CLASS_TOUCHSCREEN || cls > CLASS_GAMEPAD)
                continue;
            String flags = fields[4];
            devices.add(new DiscoveredDevice(fields[0], fields[1], cls, bus,
                    flags.contains("a"), flags.contains("w")));
        }
        Collections.sort(devices, BY_NODE_NUMBER);
        return devices;
    }

    /**
     * Enumerates every input node through the root helper. Nothing is grabbed
     * and no session is disturbed, so this is safe to call while one is running:
     * a node the session was told to leave alone still shows up here.
     *
     * <p>Blocking — callers must run this off the main thread.
     *
     * @return the nodes found, or {@code null} when the helper could not be asked
     *         at all. That is not the same as an empty list, and the caller has
     *         to say so rather than showing an empty device page.
     */
    static List<DiscoveredDevice> discoverDevices(Context context) {
        String helper = context.getApplicationInfo().nativeLibraryDir + "/libinputgrab.so";
        SuCommand.CommandResult result =
                new SuCommand.SuRunner().run(helper + " --list", DISCOVERY_TIMEOUT_MS);
        if (!result.isClean()) {
            Log.w(TAG, "input discovery failed: exit=" + result.exitCode
                    + " timeout=" + result.timedOut + " truncated=" + result.truncated
                    + " unavailable=" + result.unavailable + " stderr=" + result.stderr);
            return null;
        }
        return parseDeviceList(result.stdout);
    }

    private final Context context;
    private final InputGrabTransport.Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private LocalSocket binder;          // owns the listening fd; closed via `server`
    private volatile LocalServerSocket server;
    private volatile LocalSocket client;
    private Thread heartbeat;

    private volatile boolean running = false;
    private volatile boolean stopping = false;
    /**
     * Token of a session whose helper is known to be gone, so the last-resort
     * kill is skipped. Per-token rather than a flag: a delayed kill from the
     * previous session must never target the one that replaced it.
     */
    private volatile String finishedToken = null;
    private String cleanupToken; // guarded by killHelper; not proof that the helper has exited
    private volatile long lastMainAlive = 0L;
    private boolean endReported = false;
    private volatile String killToken;

    InputGrab(Context context, InputGrabTransport.Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    @Override
    public boolean isRunning() {
        return running;
    }

    @Override
    public boolean start(int toggleScanCode) {
        return start(toggleScanCode, null, null);
    }

    @Override
    public boolean start(int toggleScanCode, Collection<String> selectedNodes) {
        return start(toggleScanCode, selectedNodes, null);
    }

    /**
     * Launch a session.
     *
     * @param selectedNodes nodes to take, or null/empty to auto-select
     * @param excludedNodes nodes to leave alone, normally the ones Gold holds
     * @return false when the arguments cannot be honoured or the listening socket
     *         could not be created; every later failure arrives through
     *         {@link InputGrabTransport.Listener#onEnded}.
     */
    @Override
    public boolean start(int toggleScanCode, Collection<String> selectedNodes,
                         Collection<String> excludedNodes) {
        return start(toggleScanCode, selectedNodes, excludedNodes,
                ImmersiveInputSource.DIRECT_EVENT_NODES);
    }

    @Override
    public boolean start(int toggleScanCode, Collection<String> selectedNodes,
                         Collection<String> excludedNodes, ImmersiveInputSource source) {
        if (running)
            return true;
        if (source == null || toggleScanCode < 0
                || (toggleScanCode == 0 && !source.allowsUnboundToggle()))
            return false;

        // Built here, not in the worker: an unusable node list has to fail the
        // start rather than quietly launch a session that ignores it.
        final String nodesArgument = serializeSelectedNodes(selectedNodes);
        final String excludeArgument = serializeExcludedNodes(excludedNodes);
        if (nodesArgument == null || excludeArgument == null) {
            Log.w(TAG, "refusing to start with an unusable node list");
            return false;
        }

        killToken = Long.toHexString(SystemClock.elapsedRealtimeNanos())
                + Integer.toHexString(System.identityHashCode(this));
        final String token = killToken;
        final String socketName = "anland.igrab." + token;

        try {
            binder = new LocalSocket(LocalSocket.SOCKET_STREAM);
            // Abstract namespace (LocalServerSocket's own default): no file to
            // create, chmod or leave behind, and root needs one SELinux
            // permission fewer than for a socket in the app's cache dir. The name
            // carries a random token and the peer's uid is checked on accept, so
            // another app cannot slip into the event stream.
            binder.bind(new LocalSocketAddress(socketName,
                    LocalSocketAddress.Namespace.ABSTRACT));
            server = new LocalServerSocket(binder.getFileDescriptor());
        } catch (IOException e) {
            Log.e(TAG, "cannot listen on " + socketName, e);
            closeAll();
            return false;
        }

        running = true;
        stopping = false;
        finishedToken = null;
        endReported = false;
        lastMainAlive = SystemClock.uptimeMillis();
        main.postDelayed(mainBeacon, MAIN_BEACON_INTERVAL_MS);

        final String helper = context.getApplicationInfo().nativeLibraryDir
                + "/libinputgrab.so";
        Thread worker = new Thread(() -> run(helper, socketName, toggleScanCode, token,
                nodesArgument, excludeArgument, source.helperArgument()), "anland-inputgrab");
        worker.setDaemon(true);
        worker.start();
        return true;
    }

    /** End the session. Idempotent; {@code onEnded} still fires once. */
    @Override
    public void stop() {
        if (!running)
            return;
        stopping = true;
        final String token = killToken;
        // Closing the socket is the release that matters: the helper's poll()
        // sees EOF and ungrabs. Everything else here is a backstop.
        closeAll();
        killHelper(token); // Cleanup waits off the main thread before sending any signal.
    }

    // ---- main-thread liveness beacon -------------------------------------

    // Re-posts itself while a session is live. The heartbeat writer only sends
    // when this ran recently, so a wedged main thread stops the heartbeat and the
    // helper hands the input back without needing anything from us.
    private final Runnable mainBeacon = new Runnable() {
        @Override
        public void run() {
            if (!running)
                return;
            lastMainAlive = SystemClock.uptimeMillis();
            main.postDelayed(this, MAIN_BEACON_INTERVAL_MS);
        }
    };

    // ---- worker ----------------------------------------------------------

    private void run(String helperPath, String socketName, int toggleScanCode,
                     String token, String nodesArgument, String excludeArgument,
                     String sourceArgument) {
        int reason = REASON_NO_ROOT;
        Process p = null;
        try {
            // The token is inert to the helper (it parses only the keywords it
            // knows); it exists so this session can find its own process again
            // later. The node arguments are appended only when present, so a
            // session with no selection still produces the original command.
            StringBuilder command = new StringBuilder();
            command.append(helperPath).append(' ').append(socketName).append(' ')
                    .append(toggleScanCode).append(" igrabtoken=").append(token)
                    .append(' ').append(sourceArgument);
            if (!nodesArgument.isEmpty())
                command.append(' ').append(nodesArgument);
            if (!excludeArgument.isEmpty())
                command.append(' ').append(excludeArgument);
            command.append(" apk=").append(SuCommand.shellQuote(
                    context.getApplicationInfo().sourceDir));
            command.append(" >/dev/null 2>&1");
            String cmd = command.toString();
            try {
                p = new ProcessBuilder("su", "-c", cmd)
                        .redirectErrorStream(true).start();
            } catch (IOException e) {
                // `su` is missing or refused outright: nothing was launched, so
                // nothing can be holding a grab and no kill is needed.
                Log.w(TAG, "su failed: " + e);
                finishedToken = token;
                return;
            }
            drain(p);

            LocalSocket sock = acceptHelper();
            if (sock == null) {
                // Nothing was ever grabbed: a helper that starts late finds the
                // abstract name gone, fails to connect and exits before it
                // touches any device.
                finishedToken = token;
                return;
            }
            client = sock;
            if (stopping)
                return;
            startHeartbeat(sock);
            reason = readLoop(sock, token);
        } catch (IOException e) {
            // The stream broke with the helper possibly still grabbing; the kill
            // in the finally block is what covers that.
            Log.w(TAG, "immersive stream ended: " + e);
            reason = REASON_IO;
        } catch (Throwable t) {
            Log.e(TAG, "immersive session crashed", t);
            reason = REASON_IO;
        } finally {
            final int r = stopping ? REASON_STOPPED : reason;
            closeAll();
            killHelper(token);
            // Do not destroy the su process while its helper is still restoring.
            // EOF and the delayed TERM/KILL cleanup below let it exit naturally.
            if (p != null && !p.isAlive())
                p.destroy();
            main.post(() -> finish(r));
        }
    }

    /** Discard the su pipe so a chatty `su` can never block on a full one. */
    private void drain(final Process p) {
        Thread t = new Thread(() -> {
            byte[] scratch = new byte[256];
            try (InputStream in = p.getInputStream()) {
                while (in.read(scratch) >= 0) {
                    // discarded on purpose: the helper logs through liblog
                }
            } catch (IOException ignored) {
            }
        }, "anland-inputgrab-drain");
        t.setDaemon(true);
        t.start();
    }

    /**
     * Wait for the helper's connection, verifying it really is root: the abstract
     * name is reachable process-wide, so an unprivileged peer is refused rather
     * than handed the device's entire input stream.
     */
    private LocalSocket acceptHelper() {
        final LocalServerSocket srv = server;
        if (srv == null)
            return null;
        // LocalServerSocket has no accept timeout; close it from a timer instead.
        // Closing an fd on Android signals threads blocked on it, so the pending
        // accept() returns. Guarded on identity so a timer left over from an
        // earlier session cannot close this one's listener.
        main.postDelayed(() -> {
            if (server == srv && client == null)
                closeServer();
        }, CONNECT_TIMEOUT_MS);
        try {
            LocalSocket sock = srv.accept();
            Credentials cred = sock.getPeerCredentials();
            if (cred == null || cred.getUid() != 0) {
                Log.e(TAG, "rejecting non-root peer uid="
                        + (cred == null ? -1 : cred.getUid()));
                sock.close();
                return null;
            }
            // One peer only. Dropping the listener also releases the abstract
            // name, so nothing else can connect for the rest of the session.
            closeServer();
            return sock;
        } catch (IOException e) {
            if (!stopping)
                Log.w(TAG, "helper did not connect: " + e);
            return null;
        }
    }

    private void startHeartbeat(final LocalSocket sock) {
        Thread hb = new Thread(() -> {
            try {
                OutputStream out = sock.getOutputStream();
                byte[] beat = {(byte) 0xA5};
                while (running && !stopping) {
                    Thread.sleep(HEARTBEAT_INTERVAL_MS);
                    if (SystemClock.uptimeMillis() - lastMainAlive
                            > MAIN_ALIVE_WINDOW_MS) {
                        // Withholding the beat is the point: a main thread that
                        // stopped running cannot end the session, so let the
                        // helper time out and release the devices itself.
                        Log.w(TAG, "main thread stalled; withholding heartbeat");
                        continue;
                    }
                    out.write(beat);
                    out.flush();
                }
            } catch (InterruptedException ignored) {
            } catch (IOException e) {
                // Socket already gone; the read loop reports the end.
            }
        }, "anland-inputgrab-hb");
        heartbeat = hb;
        hb.setDaemon(true);
        hb.start();
    }

    /**
     * Parse the fixed-size record stream. Records carry no length field precisely
     * so that the helper can drop them under back-pressure without ever
     * desyncing this parser.
     */
    private int readLoop(LocalSocket sock, String token) throws IOException {
        InputStream in = sock.getInputStream();
        byte[] buf = new byte[REC_SIZE * 64];
        ByteBuffer bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        int have = 0;
        int[] batch = new int[512];
        int batchLen = 0;

        while (!stopping) {
            int n = in.read(buf, have, buf.length - have);
            if (n < 0)
                return REASON_PEER_GONE;
            have += n;

            int off = 0;
            while (have - off >= REC_SIZE) {
                final int rec = off;
                off += REC_SIZE;

                int rtype = bb.getShort(rec) & 0xFFFF;
                int dev   = bb.getShort(rec + 2) & 0xFFFF;
                int etype = bb.getShort(rec + 4) & 0xFFFF;
                int code  = bb.getShort(rec + 6) & 0xFFFF;
                int value = bb.getInt(rec + 8);

                switch (rtype) {
                    case REC_HELLO:
                        if (value != PROTO_VERSION) {
                            Log.e(TAG, "unsupported input helper protocol " + value);
                            return REASON_IO;
                        }
                        Log.i(TAG, "helper v" + value + " pid=" + bb.getInt(rec + AUX0 + 4)
                                + " grabbed=" + bb.getInt(rec + AUX0));
                        break;
                    case REC_DEVICE: {
                        // A hotplug replacement is preceded by a global
                        // SYN_DROPPED. Post that batch before replacing devs[d],
                        // otherwise the main thread can cancel old state through
                        // the new device object when both records share a read.
                        batchLen = flush(batch, batchLen);
                        final int d = dev, cls = etype;
                        final int minX = bb.getInt(rec + AUX0);
                        final int maxX = bb.getInt(rec + AUX0 + 4);
                        final int minY = bb.getInt(rec + AUX0 + 8);
                        final int maxY = bb.getInt(rec + AUX0 + 12);
                        final int flags = bb.getInt(rec + AUX0 + 16);
                        main.post(() -> {
                            if (running && !stopping)
                                listener.onDevice(d, cls, minX, maxX, minY, maxY, flags);
                        });
                        break;
                    }
                    case REC_AXIS_RANGE: {
                        batchLen = flush(batch, batchLen);
                        final int d = dev, axis = code, min = value;
                        final int max = bb.getInt(rec + AUX0);
                        main.post(() -> {
                            if (running && !stopping)
                                listener.onAxisRange(d, axis, min, max);
                        });
                        break;
                    }
                    case REC_READY: {
                        batchLen = flush(batch, batchLen);
                        final int grabbed = bb.getInt(rec + AUX0);
                        main.post(() -> {
                            if (running && !stopping)
                                listener.onReady(grabbed);
                        });
                        break;
                    }
                    case REC_EVENT:
                        if (batchLen + EVENT_STRIDE > batch.length)
                            batchLen = flush(batch, batchLen);
                        batch[batchLen++] = dev;
                        batch[batchLen++] = etype;
                        batch[batchLen++] = code;
                        batch[batchLen++] = value;
                        batch[batchLen++] = bb.getInt(rec + AUX0);
                        batch[batchLen++] = bb.getInt(rec + AUX0 + 4);
                        if (etype == 0 && code == 0)   // EV_SYN / SYN_REPORT
                            batchLen = flush(batch, batchLen);
                        break;
                    case REC_BYE:
                        batchLen = flush(batch, batchLen);
                        // The helper only sends this on its way out, so no kill
                        // is needed for this session.
                        finishedToken = token;
                        return ((value >= REASON_TOGGLE && value <= REASON_HELPER_ERR)
                                || value == REASON_CONTROLLER) ? value : REASON_PEER_GONE;
                    default:
                        Log.e(TAG, "bad record type " + rtype + "; ending session");
                        flush(batch, batchLen);
                        return REASON_IO;
                }
            }

            if (off > 0 && off < have)
                System.arraycopy(buf, off, buf, 0, have - off);
            have -= off;
            // Nothing left to parse: push what is pending rather than sit on it
            // waiting for a SYN that a key-only device never sends.
            batchLen = flush(batch, batchLen);
        }
        return REASON_STOPPED;
    }

    private int flush(int[] batch, int len) {
        if (len == 0)
            return 0;
        final int[] copy = new int[len];
        System.arraycopy(batch, 0, copy, 0, len);
        main.post(() -> {
            if (running && !stopping)
                listener.onEvents(copy, copy.length);
        });
        return 0;
    }

    // ---- teardown --------------------------------------------------------

    private void finish(int reason) {
        if (endReported)
            return;
        endReported = true;
        running = false;
        main.removeCallbacks(mainBeacon);
        listener.onEnded(reason);
    }

    private void closeServer() {
        LocalServerSocket srv = server;
        server = null;
        if (srv != null) {
            try {
                srv.close();
            } catch (IOException ignored) {
            }
        }
        // `binder` shares the listening fd with `server`; closing it a second time
        // would close whatever fd number has been reused since. Drop the
        // reference only — it exists to keep the fd alive, not to own it.
        binder = null;
    }

    private void closeAll() {
        closeServer();
        LocalSocket c = client;
        client = null;
        if (c != null) {
            try {
                // Half-close first so the helper's read() sees EOF immediately
                // and starts ungrabbing, rather than waiting on the close.
                c.shutdownOutput();
            } catch (IOException ignored) {
            }
            try {
                c.close();
            } catch (IOException ignored) {
            }
        }
        Thread hb = heartbeat;
        heartbeat = null;
        if (hb != null)
            hb.interrupt();
    }

    /**
     * Last resort when the helper did not announce its own exit. Matched on the
     * token this session put in its argv rather than on a pid, so a recycled pid
     * can never be the target; the "[x]" prefix keeps the pattern from matching
     * the shell that runs pkill.
     */
    private synchronized void killHelper(final String token) {
        if (token == null || token.isEmpty() || token.equals(finishedToken)
                || token.equals(cleanupToken))
            return;
        cleanupToken = token;
        Thread t = new Thread(() -> {
            String pattern = "igrabtoken=[" + token.charAt(0) + "]" + token.substring(1);
            try {
                Thread.sleep(KILL_FALLBACK_MS); // EOF normally completes restoration.
                for (String signal : new String[]{"TERM", "KILL"}) {
                    if (token.equals(finishedToken))
                        return;
                    Process p = null;
                    try {
                        p = new ProcessBuilder("su", "-c",
                                "pkill -" + signal + " -f '" + pattern
                                        + "' >/dev/null 2>&1").start();
                        p.waitFor(KILL_FALLBACK_MS, java.util.concurrent.TimeUnit.MILLISECONDS);
                    } finally {
                        if (p != null)
                            p.destroy();
                    }
                    // TERM is handled by the helper, so allow its cleanup to run
                    // before KILL. Native restorers have no token and survive both.
                    if ("TERM".equals(signal))
                        Thread.sleep(KILL_FALLBACK_MS);
                }
            } catch (Exception error) {
                Log.w(TAG, "helper cleanup failed", error);
            }
        }, "anland-inputgrab-kill");
        t.setDaemon(true);
        t.start();
    }
}
