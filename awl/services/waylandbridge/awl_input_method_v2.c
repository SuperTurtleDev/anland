/* awl_input_method_v2.c - zwp_input_method_v2 server side.
 *
 * This first implementation deliberately keeps the scope narrow and honest:
 * one input method per wl_seat, with the protocol's pending/current commit
 * model. It bridges committed IME state to the existing Android callback path;
 * popup surfaces and keyboard grabs are separate follow-up capabilities.
 */
#include "awl_internal.h"

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/syscall.h>

#include "input-method-unstable-v2-server-protocol.h"
#include "keymap_evdev.h"

#define AWL_IME_V2_TEXT_MAX 4000

struct awl_ime_v2 {
    struct wl_resource *res;
    struct wl_client *client;
    struct wl_list link;
    bool unavailable;
    bool active;
    struct awl_surface *popup_surface;
    struct wl_resource *popup_res;
    struct wl_resource *keyboard_grab_res;
    uint32_t done_count;

    struct {
        char commit[AWL_IME_V2_TEXT_MAX + 1];
        char preedit[AWL_IME_V2_TEXT_MAX + 1];
        int32_t pre_begin;
        int32_t pre_end;
        uint32_t delete_before;
        uint32_t delete_after;
    } pending;
};

static struct wl_list g_ime_v2;
static pthread_mutex_t g_ime_v2_lock = PTHREAD_MUTEX_INITIALIZER;
static struct awl_ime_v2 *g_active_ime;

static struct awl_ime_v2 *ime_from_res(struct wl_resource *res)
{
    return res ? wl_resource_get_user_data(res) : NULL;
}

static void ime_v2_reset_pending(struct awl_ime_v2 *ime)
{
    ime->pending.commit[0] = '\0';
    ime->pending.preedit[0] = '\0';
    ime->pending.pre_begin = 0;
    ime->pending.pre_end = 0;
    ime->pending.delete_before = 0;
    ime->pending.delete_after = 0;
}

static void ime_v2_destroy_res(struct wl_resource *res)
{
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime)
        return;

    pthread_mutex_lock(&g_ime_v2_lock);
    if (g_active_ime == ime)
        g_active_ime = NULL;
    wl_list_remove(&ime->link);
    pthread_mutex_unlock(&g_ime_v2_lock);
    free(ime);
}

static void ime_v2_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void ime_v2_commit_string(struct wl_client *client,
                                  struct wl_resource *res, const char *text)
{
    (void)client;
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime || ime->unavailable)
        return;
    snprintf(ime->pending.commit, sizeof(ime->pending.commit), "%s", text ? text : "");
}

static void ime_v2_set_preedit(struct wl_client *client,
                                struct wl_resource *res, const char *text,
                                int32_t begin, int32_t end)
{
    (void)client;
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime || ime->unavailable)
        return;
    snprintf(ime->pending.preedit, sizeof(ime->pending.preedit), "%s", text ? text : "");
    ime->pending.pre_begin = begin;
    ime->pending.pre_end = end;
}

static void ime_v2_delete_surrounding(struct wl_client *client,
                                      struct wl_resource *res,
                                      uint32_t before, uint32_t after)
{
    (void)client;
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime || ime->unavailable)
        return;
    ime->pending.delete_before = before;
    ime->pending.delete_after = after;
}

static void ime_v2_commit(struct wl_client *client, struct wl_resource *res,
                          uint32_t serial)
{
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime || ime->unavailable)
        return;

    /* A stale serial must not mutate the current state. The protocol still
     * accepts the request, so simply acknowledge it with no state change. */
    if (serial != ime->done_count) {
        zwp_input_method_v2_send_done(res);
        wl_client_flush(client);
        return;
    }

    pthread_mutex_lock(&g_ime_v2_lock);
    ime->done_count++;
    bool active = ime->active && g_active_ime == ime;
    pthread_mutex_unlock(&g_ime_v2_lock);

    /* Android-side text application is intentionally routed through the
     * existing public bridge. Delete first, then commit, then preedit. */
    if (active) {
        uint64_t win = awl_ime_focus_window();
        if (ime->pending.delete_before || ime->pending.delete_after)
            awl_ime_text(win, AWL_IME_DELETE, "",
                         (int32_t)ime->pending.delete_before,
                         (int32_t)ime->pending.delete_after);
        if (ime->pending.commit[0])
            awl_ime_text(win, AWL_IME_COMMIT, ime->pending.commit, 0, 0);
        if (ime->pending.preedit[0])
            awl_ime_text(win, AWL_IME_PREEDIT, ime->pending.preedit,
                         ime->pending.pre_begin, ime->pending.pre_end);
    }

    ime_v2_reset_pending(ime);
    zwp_input_method_v2_send_done(res);
    wl_client_flush(client);
}

static void ime_v2_popup_destroy(struct wl_client *client,
                                  struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void ime_v2_popup_res_destroy(struct wl_resource *res)
{
    struct awl_surface *s = wl_resource_get_user_data(res);
    if (!s)
        return;
    pthread_mutex_lock(&g_ime_v2_lock);
    struct awl_ime_v2 *ime;
    wl_list_for_each(ime, &g_ime_v2, link) {
        if (ime->popup_res == res) {
            ime->popup_res = NULL;
            ime->popup_surface = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_ime_v2_lock);
    pthread_mutex_lock(&s->ev_lock);
    if (s->ime_popup_res == res) {
        s->ime_popup_res = NULL;
        s->role = AWL_ROLE_NONE;
    }
    pthread_mutex_unlock(&s->ev_lock);
}

static const struct zwp_input_popup_surface_v2_interface ime_v2_popup_iface = {
    .destroy = ime_v2_popup_destroy,
};

static void ime_v2_get_popup(struct wl_client *client,
                             struct wl_resource *res, uint32_t id,
                             struct wl_resource *surface)
{
    struct awl_ime_v2 *ime = ime_from_res(res);
    struct awl_surface *s = surface ? awl_surface_from_res(surface) : NULL;
    if (!ime || ime->unavailable || !s || s->resource != surface ||
        s->role != AWL_ROLE_NONE) {
        wl_resource_post_error(res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "surface already has a role or is invalid");
        return;
    }
    pthread_mutex_lock(&g_ime_v2_lock);
    if (ime->popup_res) {
        pthread_mutex_unlock(&g_ime_v2_lock);
        wl_resource_post_error(res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "input method already has a popup surface");
        return;
    }
    struct wl_resource *popup = wl_resource_create(
        client, &zwp_input_popup_surface_v2_interface, 1, id);
    if (!popup) {
        pthread_mutex_unlock(&g_ime_v2_lock);
        wl_resource_post_no_memory(res);
        return;
    }
    pthread_mutex_lock(&s->ev_lock);
    if (s->role != AWL_ROLE_NONE) {
        pthread_mutex_unlock(&s->ev_lock);
        pthread_mutex_unlock(&g_ime_v2_lock);
        wl_resource_destroy(popup);
        wl_resource_post_error(res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "surface already has a role");
        return;
    }
    s->role = AWL_ROLE_INPUT_POPUP;
    s->ime_popup_res = popup;
    ime->popup_res = popup;
    ime->popup_surface = s;
    pthread_mutex_unlock(&s->ev_lock);
    pthread_mutex_unlock(&g_ime_v2_lock);
    wl_resource_set_implementation(popup, &ime_v2_popup_iface, s,
                                    ime_v2_popup_res_destroy);
    wl_client_flush(client);
}
static void ime_v2_grab_release(struct wl_client *client,
                                  struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void ime_v2_grab_res_destroy(struct wl_resource *res)
{
    struct awl_ime_v2 *ime = wl_resource_get_user_data(res);
    if (!ime) return;
    pthread_mutex_lock(&g_ime_v2_lock);
    if (ime->keyboard_grab_res == res)
        ime->keyboard_grab_res = NULL;
    pthread_mutex_unlock(&g_ime_v2_lock);
}

static const struct zwp_input_method_keyboard_grab_v2_interface ime_v2_grab_iface = {
    .release = ime_v2_grab_release,
};

static void ime_v2_send_keymap(struct wl_resource *grab)
{
    size_t len = strlen(k_keymap_evdev);
    int fd = (int)syscall(SYS_memfd_create, "awl-ime-keymap", MFD_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, k_keymap_evdev, len) != (ssize_t)len) {
        close(fd);
        return;
    }
    lseek(fd, 0, SEEK_SET);
    zwp_input_method_keyboard_grab_v2_send_keymap(
        grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
}

static void ime_v2_grab_keyboard(struct wl_client *client,
                                 struct wl_resource *res, uint32_t id)
{
    struct awl_ime_v2 *ime = ime_from_res(res);
    if (!ime || ime->unavailable || !ime->active) {
        wl_resource_post_error(res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "keyboard grab requires an active input method");
        return;
    }
    pthread_mutex_lock(&g_ime_v2_lock);
    if (ime->keyboard_grab_res) {
        pthread_mutex_unlock(&g_ime_v2_lock);
        wl_resource_post_error(res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "keyboard already grabbed");
        return;
    }
    struct wl_resource *grab = wl_resource_create(
        client, &zwp_input_method_keyboard_grab_v2_interface, 1, id);
    if (!grab) {
        pthread_mutex_unlock(&g_ime_v2_lock);
        wl_resource_post_no_memory(res);
        return;
    }
    ime->keyboard_grab_res = grab;
    pthread_mutex_unlock(&g_ime_v2_lock);
    wl_resource_set_implementation(grab, &ime_v2_grab_iface, ime,
                                    ime_v2_grab_res_destroy);
    ime_v2_send_keymap(grab);
    zwp_input_method_keyboard_grab_v2_send_repeat_info(grab, 25, 600);
    wl_client_flush(client);
}

int awl_ime_v2_send_key(uint64_t win, uint32_t code, uint32_t state,
                        uint32_t serial, uint32_t time,
                        uint32_t dep, uint32_t lck, int mods_changed)
{
    int sent = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface *s = awl_surface_by_id(win);
    struct wl_client *owner = (s && s->resource) ?
        wl_resource_get_client(s->resource) : NULL;
    pthread_mutex_lock(&g_ime_v2_lock);
    struct awl_ime_v2 *ime = g_active_ime;
    struct wl_resource *grab = (ime && ime->active && ime->keyboard_grab_res &&
                                ime->client == owner) ? ime->keyboard_grab_res : NULL;
    if (grab) {
        if (mods_changed)
            zwp_input_method_keyboard_grab_v2_send_modifiers(grab, serial,
                                                               dep, 0, lck, 0);
        zwp_input_method_keyboard_grab_v2_send_key(grab, serial, time, code,
                                                   state);
        wl_client_flush(owner);
        sent = 1;
    }
    pthread_mutex_unlock(&g_ime_v2_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    return sent;
}


static const struct zwp_input_method_v2_interface ime_v2_iface = {
    .commit_string = ime_v2_commit_string,
    .set_preedit_string = ime_v2_set_preedit,
    .delete_surrounding_text = ime_v2_delete_surrounding,
    .commit = ime_v2_commit,
    .get_input_popup_surface = ime_v2_get_popup,
    .grab_keyboard = ime_v2_grab_keyboard,
    .destroy = ime_v2_destroy,
};

static void ime_v2_get_input_method(struct wl_client *client,
                                    struct wl_resource *res,
                                    struct wl_resource *seat, uint32_t id)
{
    (void)seat;
    struct wl_resource *obj = wl_resource_create(
        client, &zwp_input_method_v2_interface, 1, id);
    if (!obj) {
        wl_resource_post_no_memory(res);
        return;
    }

    struct awl_ime_v2 *ime = calloc(1, sizeof(*ime));
    if (!ime) {
        wl_resource_destroy(obj);
        wl_resource_post_no_memory(res);
        return;
    }
    ime->res = obj;
    ime->client = client;
    ime->done_count = 0;
    ime_v2_reset_pending(ime);

    pthread_mutex_lock(&g_ime_v2_lock);
    wl_list_insert(g_ime_v2.prev, &ime->link);
    bool occupied = g_active_ime != NULL;
    if (!occupied)
        g_active_ime = ime;
    pthread_mutex_unlock(&g_ime_v2_lock);

    wl_resource_set_implementation(obj, &ime_v2_iface, ime, ime_v2_destroy_res);
    if (occupied) {
        ime->unavailable = true;
        zwp_input_method_v2_send_unavailable(obj);
    }
    wl_client_flush(client);
}

static void ime_v2_manager_destroy(struct wl_client *client,
                                    struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static const struct zwp_input_method_manager_v2_interface ime_v2_manager_iface = {
    .get_input_method = ime_v2_get_input_method,
    .destroy = ime_v2_manager_destroy,
};

static void ime_v2_manager_bind(struct wl_client *client, void *data,
                                uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *res = wl_resource_create(
        client, &zwp_input_method_manager_v2_interface,
        version < 1 ? version : 1, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &ime_v2_manager_iface, NULL, NULL);
}

void awl_ime_v2_setup(void)
{
    wl_list_init(&g_ime_v2);
    g_active_ime = NULL;
    if (!wl_global_create(g_srv.display,
                          &zwp_input_method_manager_v2_interface,
                          1, NULL, ime_v2_manager_bind))
        LOGE("zwp_input_method_manager_v2 global create failed");
}

void awl_ime_v2_focus_enter(uint64_t win)
{
    (void)win;
    pthread_mutex_lock(&g_ime_v2_lock);
    struct awl_ime_v2 *ime = g_active_ime;
    if (ime && !ime->unavailable && !ime->active) {
        ime->active = true;
        ime_v2_reset_pending(ime);
        zwp_input_method_v2_send_activate(ime->res);
        zwp_input_method_v2_send_done(ime->res);
        wl_client_flush(ime->client);
    }
    pthread_mutex_unlock(&g_ime_v2_lock);
}

void awl_ime_v2_focus_leave(uint64_t win)
{
    (void)win;
    pthread_mutex_lock(&g_ime_v2_lock);
    struct awl_ime_v2 *ime = g_active_ime;
    if (ime && ime->active) {
        ime->active = false;
        zwp_input_method_v2_send_deactivate(ime->res);
        zwp_input_method_v2_send_done(ime->res);
        wl_client_flush(ime->client);
    }
    pthread_mutex_unlock(&g_ime_v2_lock);
}

void awl_ime_v2_surface_gone(struct awl_surface *s)
{
    if (!s)
        return;
    /* The popup object may outlive the wl_surface resource during disconnect
     * teardown. Break both sides of the association before surface memory is
     * released; its later popup destroy callback then becomes a no-op. */
    pthread_mutex_lock(&g_ime_v2_lock);
    struct awl_ime_v2 *ime;
    wl_list_for_each(ime, &g_ime_v2, link) {
        if (ime->popup_surface == s) {
            ime->popup_surface = NULL;
            if (ime->popup_res) {
                wl_resource_set_user_data(ime->popup_res, NULL);
                ime->popup_res = NULL;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_ime_v2_lock);
    /* The surface is already being destroyed, so do not touch its mutex or
     * role here; surface_destroy_impl owns that teardown ordering. */
}