#include "anland_present.h"
#include "anland_scene_legacy.h"
#include <stdlib.h>

/* One presentation path. The scene backend retains submission, generation,
 * backpressure, fence ownership and acknowledgement/release accounting. */
struct anland_present {
    anland_scene_legacy *transport;
};

anland_present *anland_present_create(const anland_present_config_t *config)
{
    if (!config)
        return NULL;
    anland_present *present = calloc(1, sizeof(*present));
    if (!present)
        return NULL;
    present->transport = anland_scene_legacy_create(config->endpoint);
    if (!present->transport) {
        free(present);
        return NULL;
    }
    return present;
}

void anland_present_destroy(anland_present *present)
{
    if (!present)
        return;
    anland_scene_legacy_destroy(present->transport);
    free(present);
}

anland_scene *anland_present_scene(anland_present *present)
{
    return present ? anland_scene_legacy_scene(present->transport) : NULL;
}

anland_device *anland_present_device(anland_present *present)
{
    return present ? anland_scene_legacy_device(present->transport) : NULL;
}

bool anland_present_connected(const anland_present *present)
{
    return present && anland_scene_legacy_connected(present->transport);
}

bool anland_present_target_available(const anland_present *present)
{
    return present && anland_scene_legacy_target_available(present->transport);
}

int anland_present_reconnect(anland_present *present)
{
    return present ? anland_scene_legacy_reconnect(present->transport) : -1;
}

int anland_present_reopen(anland_present *present, const char *endpoint)
{
    return present ? anland_scene_legacy_reopen(present->transport, endpoint) : -1;
}

void anland_present_drop_session(anland_present *present)
{
    if (present)
        anland_scene_legacy_drop_session(present->transport);
}

int anland_present_present(anland_present *present)
{
    return present ? anland_scene_legacy_present(present->transport) : -1;
}

int anland_present_pump(anland_present *present, int timeout_ms)
{
    return present ? anland_scene_legacy_pump(present->transport, timeout_ms) : -1;
}