#include "anland_window_map.h"

#include <pthread.h>
#include <stdlib.h>

struct anland_window_entry {
    anland_layer_id layer_id;
    uint64_t window_id;
};

struct anland_window_map {
    pthread_mutex_t lock;
    struct anland_window_entry *entries;
    size_t count;
    size_t capacity;
};

static int reserve_one(anland_window_map *map)
{
    if (map->count < map->capacity)
        return 0;

    size_t capacity = map->capacity ? map->capacity * 2 : 8;
    struct anland_window_entry *entries =
        realloc(map->entries, capacity * sizeof(*entries));
    if (!entries)
        return -1;

    map->entries = entries;
    map->capacity = capacity;
    return 0;
}

static size_t find_layer_unlocked(const anland_window_map *map,
                                  anland_layer_id layer_id)
{
    for (size_t i = 0; i < map->count; i++)
        if (map->entries[i].layer_id == layer_id)
            return i;
    return SIZE_MAX;
}

static size_t find_window_unlocked(const anland_window_map *map,
                                   uint64_t window_id)
{
    for (size_t i = 0; i < map->count; i++)
        if (map->entries[i].window_id == window_id)
            return i;
    return SIZE_MAX;
}

anland_window_map *anland_window_map_create(void)
{
    anland_window_map *map = calloc(1, sizeof(*map));
    if (!map)
        return NULL;
    if (pthread_mutex_init(&map->lock, NULL) != 0) {
        free(map);
        return NULL;
    }
    return map;
}

void anland_window_map_destroy(anland_window_map *map)
{
    if (!map)
        return;
    pthread_mutex_destroy(&map->lock);
    free(map->entries);
    free(map);
}

int anland_window_map_bind(anland_window_map *map,
                           anland_layer_id layer_id,
                           uint64_t window_id)
{
    if (!map || layer_id == 0 || window_id == 0)
        return -1;

    pthread_mutex_lock(&map->lock);
    if (find_layer_unlocked(map, layer_id) != SIZE_MAX ||
        find_window_unlocked(map, window_id) != SIZE_MAX ||
        reserve_one(map) != 0) {
        pthread_mutex_unlock(&map->lock);
        return -1;
    }

    map->entries[map->count++] = (struct anland_window_entry) {
        .layer_id = layer_id,
        .window_id = window_id,
    };
    pthread_mutex_unlock(&map->lock);
    return 0;
}

int anland_window_map_unbind_layer(anland_window_map *map,
                                   anland_layer_id layer_id,
                                   uint64_t *out_window_id)
{
    if (!map || layer_id == 0)
        return -1;

    pthread_mutex_lock(&map->lock);
    size_t index = find_layer_unlocked(map, layer_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock(&map->lock);
        return -1;
    }
    if (out_window_id)
        *out_window_id = map->entries[index].window_id;
    map->entries[index] = map->entries[--map->count];
    pthread_mutex_unlock(&map->lock);
    return 0;
}

int anland_window_map_unbind_window(anland_window_map *map,
                                    uint64_t window_id,
                                    anland_layer_id *out_layer_id)
{
    if (!map || window_id == 0)
        return -1;

    pthread_mutex_lock(&map->lock);
    size_t index = find_window_unlocked(map, window_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock(&map->lock);
        return -1;
    }
    if (out_layer_id)
        *out_layer_id = map->entries[index].layer_id;
    map->entries[index] = map->entries[--map->count];
    pthread_mutex_unlock(&map->lock);
    return 0;
}

int anland_window_map_lookup_layer(const anland_window_map *map,
                                   anland_layer_id layer_id,
                                   uint64_t *out_window_id)
{
    if (!map || layer_id == 0 || !out_window_id)
        return -1;

    pthread_mutex_lock((pthread_mutex_t *)&map->lock);
    size_t index = find_layer_unlocked(map, layer_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock((pthread_mutex_t *)&map->lock);
        return -1;
    }
    *out_window_id = map->entries[index].window_id;
    pthread_mutex_unlock((pthread_mutex_t *)&map->lock);
    return 0;
}

int anland_window_map_lookup_window(const anland_window_map *map,
                                    uint64_t window_id,
                                    anland_layer_id *out_layer_id)
{
    if (!map || window_id == 0 || !out_layer_id)
        return -1;

    pthread_mutex_lock((pthread_mutex_t *)&map->lock);
    size_t index = find_window_unlocked(map, window_id);
    if (index == SIZE_MAX) {
        pthread_mutex_unlock((pthread_mutex_t *)&map->lock);
        return -1;
    }
    *out_layer_id = map->entries[index].layer_id;
    pthread_mutex_unlock((pthread_mutex_t *)&map->lock);
    return 0;
}

size_t anland_window_map_count(const anland_window_map *map)
{
    if (!map)
        return 0;
    pthread_mutex_lock((pthread_mutex_t *)&map->lock);
    size_t count = map->count;
    pthread_mutex_unlock((pthread_mutex_t *)&map->lock);
    return count;
}

void anland_window_map_clear(anland_window_map *map)
{
    if (!map)
        return;
    pthread_mutex_lock(&map->lock);
    map->count = 0;
    pthread_mutex_unlock(&map->lock);
}