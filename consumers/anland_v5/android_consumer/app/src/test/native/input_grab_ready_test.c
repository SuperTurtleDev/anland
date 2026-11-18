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

static int check_shared_toggle(void)
{
    int events[2], sockets[2];
    if (pipe(events) < 0)
        return 1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        close(events[0]);
        close(events[1]);
        return 1;
    }

    struct input_event press[] = {
        {.type = EV_KEY, .code = KEY_A, .value = 1},
        {.time = {.tv_sec = 5000000, .tv_usec = 123456},
         .type = EV_KEY, .code = KEY_B, .value = 1},
    };
    g_toggle_watches[0] = (struct toggle_watch){.fd = events[0], .monotonic_events = 1};
    int failed = !parse_toggle_chord("30,48", KEY_A);
    failed |= write(events[1], press, sizeof(press)) != sizeof(press);
    close(events[1]);
    failed |= pump_toggle_watch(0, KEY_A) != 0;
    failed |= send_bye(sockets[0], IGRAB_BYE_TOGGLE) < 0;
    close(sockets[0]);

    struct igrab_rec bye = {0};
    failed |= recv(sockets[1], &bye, sizeof(bye), MSG_WAITALL) != sizeof(bye);
    uint64_t event_time = (uint32_t)bye.aux[IGRAB_BYE_TIME_LO]
            | ((uint64_t)(uint32_t)bye.aux[IGRAB_BYE_TIME_HI] << 32);
    failed |= bye.rtype != IGRAB_REC_BYE || bye.value != IGRAB_BYE_TOGGLE
            || bye.code != KEY_B || event_time != 5000000123ULL;
    close(sockets[1]);
    drop_toggle_watch(0);

    printf("%s read-only exit reports the completing scan code and event time\n",
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
    g_devs[0].gold = 0;
    failures += check_handshake("held physical keyboard delays READY until handoff", 0);
    g_devs[0].gold = 1;
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
    failures += check_shared_toggle();
    return failures ? 1 : 0;
}
