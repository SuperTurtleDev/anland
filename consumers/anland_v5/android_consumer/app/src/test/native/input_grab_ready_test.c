// Exercise real handshake records without Android or physical input devices.
#define main input_grab_helper_main
#include "../../main/jni/input_grab.c"
#undef main

static int check_handshake(const char *label, int expect_ready)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        return 1;
    int result = send_device_list(sockets[0], controlled_device_count());
    close(sockets[0]);
    int ready = 0, hello = 0, devices = 0;
    struct igrab_rec rec;
    ssize_t bytes;
    while ((bytes = recv(sockets[1], &rec, sizeof(rec), MSG_WAITALL)) == sizeof(rec)) {
        ready += rec.rtype == IGRAB_REC_READY;
        hello += rec.rtype == IGRAB_REC_HELLO;
        devices += rec.rtype == IGRAB_REC_DEVICE;
    }
    close(sockets[1]);
    int failed = result < 0 || bytes != 0 || hello != 1 || devices != g_ndevs
            || ready != expect_ready;
    printf("%s %s\n", failed ? "FAIL" : "ok  ", label);
    return failed;
}

static int shortcut_key(struct toggle_state *state, int code, int value)
{
    struct input_event event = {.type = EV_KEY, .code = code, .value = value};
    return toggle_matches(state, -1, &event, KEY_A);
}

static int check_toggle_shortcut(void)
{
    struct toggle_state keyboard = {0}, other_keyboard = {0};
    int failed = !shortcut_key(&keyboard, KEY_A, 1);
    failed |= parse_toggle_chord("30,30", KEY_A);
    failed |= parse_toggle_chord("30,48,46,32", KEY_A);
    failed |= parse_toggle_chord("30", KEY_A);
    failed |= parse_toggle_chord("30,0", KEY_A);
    failed |= parse_toggle_chord("30,48", KEY_B);
    failed |= !parse_toggle_chord("30,48", KEY_A);
    failed |= shortcut_key(&keyboard, KEY_B, 1);
    failed |= !shortcut_key(&keyboard, KEY_A, 1);
    failed |= shortcut_key(&keyboard, KEY_A, 1);
    failed |= shortcut_key(&keyboard, KEY_A, 0);
    failed |= !shortcut_key(&keyboard, KEY_A, 1);

    keyboard = (struct toggle_state){0};
    failed |= !parse_toggle_chord("30,48,46", KEY_A);
    failed |= shortcut_key(&keyboard, KEY_C, 1);
    failed |= shortcut_key(&keyboard, KEY_A, 1);
    failed |= shortcut_key(&other_keyboard, KEY_B, 1);
    failed |= shortcut_key(&keyboard, KEY_B, 2);
    failed |= !shortcut_key(&keyboard, KEY_B, 1);
    printf("%s one to three keys, press order, repeats, and device isolation\n",
           failed ? "FAIL" : "ok  ");
    return failed;
}

int main(void)
{
    int failures = 0;
    g_ndevs = 1;
    g_devs[0] = (struct dev_entry){.fd = 10, .lease_fd = -1, .gold = 1,
                                 .waiting_idle = 1, .cls = IGRAB_CLASS_KEYBOARD};
    failures += check_handshake("held Gold output does not announce READY", 0);
    g_devs[0].waiting_idle = 0;
    g_devs[0].grabbed = 1;
    failures += check_handshake("grabbed Gold output announces READY", 1);
    g_devs[0].gold = 0;
    g_devs[0].lease_fd = 11;
    g_devs[0].passthrough = 0;
    failures += check_handshake("pending controller lease still blocks READY", 0);
    g_devs[0].lease_fd = -1;
    failures += check_handshake("physical input keeps its normal READY handshake", 1);
    failures += check_toggle_shortcut();
    return failures ? 1 : 0;
}
