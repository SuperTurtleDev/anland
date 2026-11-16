/* Exercise the real device adapter with a deterministic transport boundary.
 * Unused adapter entry points are discarded by the test linker's section GC. */
#include "../anland_device.c"
#include <assert.h>
#include <limits.h>

static bool transport_fallback;
static int transport_connect_result;
static int transport_input_result;
static struct InputEvent transport_input;

bool is_fallback(display_ctx *ctx)
{
    (void)ctx;
    return transport_fallback;
}

int try_exit_fallback(display_ctx *ctx)
{
    (void)ctx;
    if (transport_connect_result == 0)
        transport_fallback = false;
    return transport_connect_result;
}

void force_fallback(display_ctx *ctx)
{
    (void)ctx;
    transport_fallback = true;
}

int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    if (transport_input_result > 0)
        *event = transport_input;
    return transport_input_result;
}

static void input(anland_device *dev, uint32_t type, uint32_t action, int32_t value)
{
    memset(&transport_input, 0xa5, sizeof(transport_input));
    transport_input.type = type;
    transport_input.input_action.action = action;
    transport_input.input_action.value = value;
    transport_input_result = 1;
    anland_device_input_t delivered;
    assert(anland_device_poll_input(dev, &delivered, 0) == 1);
    assert(memcmp(&delivered, &transport_input, sizeof(transport_input)) == 0);
}

int main(void)
{
    _Static_assert(sizeof(struct InputEvent) == 20, "existing input wire size");
    anland_device dev = {.hdr_sdr_white_nits = 203, .hdr_content_gain_percent = 100, .format = 43};
    assert(anland_device_hdr_sdr_white_nits(NULL) == 203);
    assert(anland_device_hdr_content_gain_percent(NULL) == 100);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_SDR_WHITE, 400);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 400);
    assert(dev.format == 43);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_CONTENT_GAIN, 150);
    assert(anland_device_hdr_content_gain_percent(&dev) == 150);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 400);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_CONTENT_GAIN, INT_MIN);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_CONTENT_GAIN, 49);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_CONTENT_GAIN, 201);
    input(&dev, INPUT_TYPE_DISPLAY_REFRESH, ANLAND_HDR_ACTION_CONTENT_GAIN, 200);
    assert(anland_device_hdr_content_gain_percent(&dev) == 150);
    input(&dev, INPUT_TYPE_ACTION, 123, 300);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_SDR_WHITE, INT_MIN);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_SDR_WHITE, 501);
    input(&dev, INPUT_TYPE_DISPLAY_REFRESH, ANLAND_HDR_ACTION_SDR_WHITE, 300);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 400);
    assert(anland_device_hdr_content_gain_percent(&dev) == 150);
    transport_input_result = 0;
    anland_device_input_t delivered;
    assert(anland_device_poll_input(&dev, &delivered, 0) == 0);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 400);

    // Maintenance polls of an already active connector must keep its setting.
    transport_fallback = false;
    transport_connect_result = 0;
    assert(anland_device_connect(&dev) == 0);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 400);
    assert(anland_device_hdr_content_gain_percent(&dev) == 150);
    // An automatically detected disconnect gets a fresh default on reconnect.
    transport_fallback = true;
    assert(anland_device_connect(&dev) == 0);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 203);
    assert(anland_device_hdr_content_gain_percent(&dev) == 100);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_CONTENT_GAIN, 50);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_SDR_WHITE, 300);
    assert(anland_device_hdr_content_gain_percent(&dev) == 50);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 300);
    input(&dev, INPUT_TYPE_ACTION, ANLAND_HDR_ACTION_SDR_WHITE, 80);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 80);
    anland_device_force_fallback(&dev);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 203);
    assert(anland_device_hdr_content_gain_percent(&dev) == 100);
    transport_connect_result = -1;
    assert(anland_device_connect(&dev) == -1);
    assert(anland_device_hdr_sdr_white_nits(&dev) == 203);
    puts("HDR device control: unchanged wire event, validation, no-op poll and reconnect reset passed");
    return 0;
}
