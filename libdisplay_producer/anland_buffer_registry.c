#include "anland_buffer_registry.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

struct anland_buffer_entry {
    anland_buffer_desc_t desc;
};

struct anland_buffer_registry {
    pthread_mutex_t lock;
    struct anland_buffer_entry *entries;
    size_t count;
    size_t capacity;
};

static size_t find_index(const anland_buffer_registry *registry, uint64_t id)
{
    for (size_t i = 0; i < registry->count; i++)
        if (registry->entries[i].desc.buffer_id == id)
            return i;
    return SIZE_MAX;
}

static int reserve_one(anland_buffer_registry *registry)
{
    if (registry->count < registry->capacity)
        return 0;
    size_t next = registry->capacity ? registry->capacity * 2 : 8;
    void *p = realloc(registry->entries, next * sizeof(*registry->entries));
    if (!p)
        return -1;
    registry->entries = p;
    registry->capacity = next;
    return 0;
}

anland_buffer_registry *anland_buffer_registry_create(void)
{
    anland_buffer_registry *registry = calloc(1, sizeof(*registry));
    if (!registry)
        return NULL;
    if (pthread_mutex_init(&registry->lock, NULL) != 0) {
        free(registry);
        return NULL;
    }
    return registry;
}

void anland_buffer_registry_clear(anland_buffer_registry *registry)
{
    if (!registry)
        return;
    pthread_mutex_lock(&registry->lock);
    for (size_t i = 0; i < registry->count; i++)
        if (registry->entries[i].desc.fd >= 0)
            close(registry->entries[i].desc.fd);
    registry->count = 0;
    pthread_mutex_unlock(&registry->lock);
}

void anland_buffer_registry_destroy(anland_buffer_registry *registry)
{
    if (!registry)
        return;
    anland_buffer_registry_clear(registry);
    pthread_mutex_destroy(&registry->lock);
    free(registry->entries);
    free(registry);
}

int anland_buffer_registry_put(anland_buffer_registry *registry,
                               const anland_buffer_desc_t *desc)
{
    if (!registry || !desc || desc->buffer_id == 0 || desc->window_id == 0 ||
        desc->fd < 0 || desc->width == 0 || desc->height == 0 ||
        desc->stride < desc->width)
        return -1;

    pthread_mutex_lock(&registry->lock);
    size_t index = find_index(registry, desc->buffer_id);
    if (index == SIZE_MAX) {
        if (reserve_one(registry) != 0) {
            pthread_mutex_unlock(&registry->lock);
            return -1;
        }
        index = registry->count++;
    } else if (registry->entries[index].desc.fd >= 0) {
        close(registry->entries[index].desc.fd);
    }
    registry->entries[index].desc = *desc;
    pthread_mutex_unlock(&registry->lock);
    return 0;
}

int anland_buffer_registry_take(anland_buffer_registry *registry,
                                uint64_t buffer_id,
                                anland_buffer_desc_t *out)
{
    if (!registry || buffer_id == 0 || !out)
        return -1;
    pthread_mutex_lock(&registry->lock);
    size_t index = find_index(registry, buffer_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock(&registry->lock);
        return -1;
    }
    *out = registry->entries[index].desc;
    registry->entries[index] = registry->entries[--registry->count];
    pthread_mutex_unlock(&registry->lock);
    return 0;
}

int anland_buffer_registry_contains(const anland_buffer_registry *registry,
                                    uint64_t buffer_id)
{
    if (!registry || buffer_id == 0)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&registry->lock);
    int found = find_index(registry, buffer_id) != SIZE_MAX;
    pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
    return found;
}

int anland_buffer_registry_frame_id(const anland_buffer_registry *registry,
                                    uint64_t buffer_id, uint64_t *out_frame_id)
{
    if (!registry || buffer_id == 0 || !out_frame_id)
        return -1;
    pthread_mutex_lock((pthread_mutex_t *)&registry->lock);
    size_t index = find_index(registry, buffer_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
        return -1;
    }
    *out_frame_id = registry->entries[index].desc.frame_id;
    pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
    return 0;
}

int anland_buffer_registry_get(const anland_buffer_registry *registry,
                               uint64_t buffer_id,
                               anland_buffer_desc_t *out)
{
    if (!registry || buffer_id == 0 || !out)
        return -1;
    pthread_mutex_lock((pthread_mutex_t *)&registry->lock);
    size_t index = find_index(registry, buffer_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
        return -1;
    }
    *out = registry->entries[index].desc;
    pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
    return 0;
}

int anland_buffer_registry_dup_fd(const anland_buffer_registry *registry,
                                  uint64_t buffer_id)
{
    anland_buffer_desc_t desc;
    if (anland_buffer_registry_get(registry, buffer_id, &desc) != 0 || desc.fd < 0)
        return -1;
    return fcntl(desc.fd, F_DUPFD_CLOEXEC, 3);
}

int anland_buffer_registry_remove(anland_buffer_registry *registry,
                                  uint64_t buffer_id)
{
    anland_buffer_desc_t desc;
    if (anland_buffer_registry_take(registry, buffer_id, &desc) != 0)
        return -1;
    if (desc.fd >= 0)
        close(desc.fd);
    return 0;
}

size_t anland_buffer_registry_count(const anland_buffer_registry *registry)
{
    if (!registry)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&registry->lock);
    size_t count = registry->count;
    pthread_mutex_unlock((pthread_mutex_t *)&registry->lock);
    return count;
}