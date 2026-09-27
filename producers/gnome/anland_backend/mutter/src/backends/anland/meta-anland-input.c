/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-anland-input.h"

#include "backends/meta-backend-private.h"
#ifdef ANLAND_MUTTER_50
#include "backends/meta-logical-monitor-private.h"
#else
#include "backends/meta-logical-monitor.h"
#endif
#include "backends/meta-monitor-manager-private.h"
#include "backends/native/meta-virtual-input-device-native.h"

#include <linux/input.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANLAND_MAX_TOUCH_BATCH_EVENTS 128
#define ANLAND_TOUCH_BATCH_TIMEOUT_MS 100

struct _MetaAnlandInput
{
  MetaBackend *backend;
  ClutterVirtualInputDevice *pointer;
  ClutterVirtualInputDevice *keyboard;
  ClutterVirtualInputDevice *touchscreen;
  gboolean pressed_keys[KEY_CNT];
  gboolean pressed_buttons[KEY_CNT];
  GHashTable *touch_slots;
  gboolean active_touch_slots[CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS];
  GArray *touch_batch;
  guint touch_batch_timeout_id;

  MetaAnlandInputRefreshFunc refresh_func;
  gpointer refresh_user_data;
};

static void
map_input_coordinates (MetaAnlandInput *input,
                       uint32_t         input_width,
                       uint32_t         input_height,
                       float            x,
                       float            y,
                       gboolean         is_delta,
                       float           *mapped_x,
                       float           *mapped_y)
{
  MetaMonitorManager *monitor_manager;
  MetaLogicalMonitor *logical_monitor;
  MtkRectangle layout;

  *mapped_x = x;
  *mapped_y = y;

  if (input_width == 0 || input_height == 0)
    return;

  monitor_manager = meta_backend_get_monitor_manager (input->backend);
  logical_monitor =
    meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (!logical_monitor)
    return;

  layout = meta_logical_monitor_get_layout (logical_monitor);

  /* Anland reports buffer pixels, while Clutter uses the logical layout. */
  *mapped_x = x * layout.width / input_width;
  *mapped_y = y * layout.height / input_height;

  if (!is_delta)
    {
      *mapped_x += layout.x;
      *mapped_y += layout.y;
    }
}

static gboolean
is_finite_pair (float x,
                float y)
{
  return isfinite (x) && isfinite (y);
}

static void
clear_touch_slot (MetaAnlandInput *input,
                  int              slot)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iter, input->touch_slots);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (GPOINTER_TO_UINT (value) == (guint) slot + 1)
        {
          g_hash_table_iter_remove (&iter);
          break;
        }
    }
  input->active_touch_slots[slot] = FALSE;
}

static void
cancel_touches (MetaAnlandInput *input)
{
  MetaVirtualTouchEvent events[CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS];
  guint n_events = 0;
  guint slot;

  if (!input->touchscreen)
    return;

  for (slot = 0; slot < G_N_ELEMENTS (input->active_touch_slots); slot++)
    {
      if (!input->active_touch_slots[slot])
        continue;

      events[n_events++] = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_CANCEL,
        .slot = slot,
      };
    }

  if (n_events > 0)
    meta_virtual_input_device_native_notify_touch_events (
      META_VIRTUAL_INPUT_DEVICE_NATIVE (input->touchscreen),
      g_get_monotonic_time (), events, n_events);

  if (input->touch_slots)
    g_hash_table_remove_all (input->touch_slots);
  memset (input->active_touch_slots, 0, sizeof (input->active_touch_slots));
}

static gboolean
touch_batch_timeout_cb (gpointer user_data)
{
  MetaAnlandInput *input = user_data;

  input->touch_batch_timeout_id = 0;
  if (input->touch_batch->len > 0)
    {
      g_warning ("Anland input dropped an incomplete touch frame");
      cancel_touches (input);
      g_array_set_size (input->touch_batch, 0);
    }

  return G_SOURCE_REMOVE;
}

static void
schedule_touch_batch_timeout (MetaAnlandInput *input)
{
  if (!input->touch_batch_timeout_id)
    input->touch_batch_timeout_id =
      g_timeout_add (ANLAND_TOUCH_BATCH_TIMEOUT_MS,
                     touch_batch_timeout_cb, input);
}

static int
allocate_touch_slot (MetaAnlandInput *input)
{
  guint slot;

  for (slot = 0; slot < G_N_ELEMENTS (input->active_touch_slots); slot++)
    {
      if (!input->active_touch_slots[slot])
        return slot;
    }

  return -1;
}

static void
flush_touch_batch (MetaAnlandInput *input)
{
  guint i;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }

  if (input->touch_batch->len == 0)
    return;

  meta_virtual_input_device_native_notify_touch_events (
    META_VIRTUAL_INPUT_DEVICE_NATIVE (input->touchscreen),
    g_get_monotonic_time (), (const MetaVirtualTouchEvent *) input->touch_batch->data,
    input->touch_batch->len);

  for (i = 0; i < input->touch_batch->len; i++)
    {
      const MetaVirtualTouchEvent *event =
        &g_array_index (input->touch_batch, MetaVirtualTouchEvent, i);

      if (event->type == META_VIRTUAL_TOUCH_EVENT_UP)
        clear_touch_slot (input, event->slot);
    }
  g_array_set_size (input->touch_batch, 0);
}

static void
handle_pointer_motion (MetaAnlandInput         *input,
                       const anland_device_input_t *event,
                       uint32_t                 input_width,
                       uint32_t                 input_height)
{
  float x, y, dx, dy;

  if (!is_finite_pair (event->pointer_motion.x, event->pointer_motion.y) ||
      !is_finite_pair (event->pointer_motion.dx, event->pointer_motion.dy))
    return;

  map_input_coordinates (input, input_width, input_height,
                         event->pointer_motion.x, event->pointer_motion.y,
                         FALSE, &x, &y);
  map_input_coordinates (input, input_width, input_height,
                         event->pointer_motion.dx, event->pointer_motion.dy,
                         TRUE, &dx, &dy);

  meta_virtual_input_device_native_notify_absolute_relative_motion (
    META_VIRTUAL_INPUT_DEVICE_NATIVE (input->pointer), g_get_monotonic_time (),
    x, y, dx, dy);
}

static void
handle_pointer_button (MetaAnlandInput         *input,
                       const anland_device_input_t *event)
{
  uint32_t button = event->pointer_button.button;
  gboolean pressed = event->pointer_button.pressed != 0;

  if (button >= KEY_CNT || button < BTN_LEFT || button > BTN_GEAR_UP ||
      input->pressed_buttons[button] == pressed)
    return;

  input->pressed_buttons[button] = pressed;
  clutter_virtual_input_device_notify_button (
    input->pointer, g_get_monotonic_time (), meta_evdev_button_to_clutter (button),
    pressed ? CLUTTER_BUTTON_STATE_PRESSED : CLUTTER_BUTTON_STATE_RELEASED);
}

static void
handle_pointer_axis (MetaAnlandInput         *input,
                     const anland_device_input_t *event)
{
  ClutterScrollDirection direction;
  double dx = 0.0;
  double dy = 0.0;
  int64_t discrete = event->pointer_axis.discrete;
  int steps;

  if (!isfinite (event->pointer_axis.value))
    return;

  switch (event->pointer_axis.axis)
    {
    case 0:
      dy = event->pointer_axis.value;
      direction = discrete >= 0 ? CLUTTER_SCROLL_DOWN : CLUTTER_SCROLL_UP;
      break;
    case 1:
      dx = event->pointer_axis.value;
      direction = discrete >= 0 ? CLUTTER_SCROLL_RIGHT : CLUTTER_SCROLL_LEFT;
      break;
    default:
      g_warning ("Anland input ignored invalid scroll axis %u",
                 event->pointer_axis.axis);
      return;
    }

  if (dx != 0.0 || dy != 0.0)
    clutter_virtual_input_device_notify_scroll_continuous (
      input->pointer, g_get_monotonic_time (), dx, dy,
      CLUTTER_SCROLL_SOURCE_WHEEL, CLUTTER_SCROLL_FINISHED_NONE);

  steps = MIN (llabs (discrete), 120);
  while (steps-- > 0)
    clutter_virtual_input_device_notify_discrete_scroll (
      input->pointer, g_get_monotonic_time (), direction,
      CLUTTER_SCROLL_SOURCE_WHEEL);
}

static void
handle_key (MetaAnlandInput         *input,
            const anland_device_input_t *event)
{
  uint32_t keycode = event->key.keycode;
  gboolean pressed;

  if (keycode >= KEY_CNT ||
      (event->key.action != ANLAND_DEVICE_ACTION_DOWN &&
       event->key.action != ANLAND_DEVICE_ACTION_UP))
    return;

  pressed = event->key.action == ANLAND_DEVICE_ACTION_DOWN;
  if (input->pressed_keys[keycode] == pressed)
    return;

  input->pressed_keys[keycode] = pressed;
  clutter_virtual_input_device_notify_key (
    input->keyboard, g_get_monotonic_time (), keycode,
    pressed ? CLUTTER_KEY_STATE_PRESSED : CLUTTER_KEY_STATE_RELEASED);
}

static void
handle_touch (MetaAnlandInput         *input,
              const anland_device_input_t *event,
              uint32_t                 input_width,
              uint32_t                 input_height)
{
  gpointer value;
  int slot;
  MetaVirtualTouchEvent touch_event;
  float x, y;

  if ((event->touch.action == ANLAND_DEVICE_ACTION_DOWN ||
       event->touch.action == ANLAND_DEVICE_ACTION_MOVE) &&
      !is_finite_pair (event->touch.x, event->touch.y))
    return;

  if (event->touch.action == ANLAND_DEVICE_ACTION_DOWN ||
      event->touch.action == ANLAND_DEVICE_ACTION_MOVE)
    map_input_coordinates (input, input_width, input_height,
                           event->touch.x, event->touch.y,
                           FALSE, &x, &y);

  value = g_hash_table_lookup (input->touch_slots,
                               GINT_TO_POINTER (event->touch.pointer_id));
  switch (event->touch.action)
    {
    case ANLAND_DEVICE_ACTION_DOWN:
      if (value)
        return;

      slot = allocate_touch_slot (input);
      if (slot < 0)
        {
          g_warning ("Anland input ran out of touch slots");
          return;
        }
      input->active_touch_slots[slot] = TRUE;
      g_hash_table_insert (input->touch_slots,
                           GINT_TO_POINTER (event->touch.pointer_id),
                           GUINT_TO_POINTER (slot + 1));
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_DOWN,
        .slot = slot,
        .x = x,
        .y = y,
      };
      break;
    case ANLAND_DEVICE_ACTION_MOVE:
      if (!value)
        return;
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_MOTION,
        .slot = GPOINTER_TO_UINT (value) - 1,
        .x = x,
        .y = y,
      };
      break;
    case ANLAND_DEVICE_ACTION_UP:
      if (!value)
        return;
      touch_event = (MetaVirtualTouchEvent) {
        .type = META_VIRTUAL_TOUCH_EVENT_UP,
        .slot = GPOINTER_TO_UINT (value) - 1,
      };
      break;
    default:
      return;
    }

  if (input->touch_batch->len >= ANLAND_MAX_TOUCH_BATCH_EVENTS)
    {
      g_warning ("Anland input dropped an oversized touch frame");
      cancel_touches (input);
      g_array_set_size (input->touch_batch, 0);
      return;
    }

  g_array_append_val (input->touch_batch, touch_event);
  schedule_touch_batch_timeout (input);
}

MetaAnlandInput *
meta_anland_input_new (MetaBackend               *backend,
                       MetaAnlandInputRefreshFunc  refresh_func,
                       gpointer                    user_data,
                       GError                    **error)
{
  MetaAnlandInput *input;
  ClutterSeat *seat;

  seat = meta_backend_get_default_seat (backend);
  if (!seat)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland backend has no default input seat");
      return NULL;
    }

  input = g_new0 (MetaAnlandInput, 1);
  input->backend = backend;
  input->touch_slots = g_hash_table_new (g_direct_hash, g_direct_equal);
  input->touch_batch = g_array_new (FALSE, FALSE, sizeof (MetaVirtualTouchEvent));
  input->pointer = clutter_seat_create_virtual_device (seat,
                                                        CLUTTER_POINTER_DEVICE);
  input->keyboard = clutter_seat_create_virtual_device (seat,
                                                         CLUTTER_KEYBOARD_DEVICE);
  input->touchscreen = clutter_seat_create_virtual_device (seat,
                                                            CLUTTER_TOUCHSCREEN_DEVICE);
  if (!input->pointer || !input->keyboard || !input->touchscreen)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create Anland virtual input devices");
      meta_anland_input_free (input);
      return NULL;
    }

  input->refresh_func = refresh_func;
  input->refresh_user_data = user_data;
  return input;
}

void
meta_anland_input_free (MetaAnlandInput *input)
{
  if (!input)
    return;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }
  meta_anland_input_reset (input);
  g_clear_object (&input->pointer);
  g_clear_object (&input->keyboard);
  g_clear_object (&input->touchscreen);
  g_clear_pointer (&input->touch_slots, g_hash_table_unref);
  g_clear_pointer (&input->touch_batch, g_array_unref);
  g_free (input);
}

MetaAnlandInputEventResult
meta_anland_input_handle_event (MetaAnlandInput             *input,
                                const anland_device_input_t *event,
                                uint32_t                     input_width,
                                uint32_t                     input_height)
{
  switch (event->type)
    {
    case ANLAND_DEVICE_IN_PTR_MOTION:
      handle_pointer_motion (input, event, input_width, input_height);
      break;
    case ANLAND_DEVICE_IN_PTR_BUTTON:
      handle_pointer_button (input, event);
      break;
    case ANLAND_DEVICE_IN_PTR_AXIS:
      handle_pointer_axis (input, event);
      break;
    case ANLAND_DEVICE_IN_KEY:
      handle_key (input, event);
      break;
    case ANLAND_DEVICE_IN_TOUCH:
      handle_touch (input, event, input_width, input_height);
      break;
    case ANLAND_DEVICE_IN_TOUCH_FRAME:
      flush_touch_batch (input);
      break;
    case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
      if (event->display.refresh_mhz >= 1000 &&
          event->display.refresh_mhz <= 1000000 && input->refresh_func)
        input->refresh_func (event->display.refresh_mhz,
                             input->refresh_user_data);
      break;
    case ANLAND_DEVICE_IN_CLIPBOARD:
    case ANLAND_DEVICE_IN_TEXT_INPUT:
      return META_ANLAND_INPUT_EVENT_NEEDS_PAYLOAD;
    case ANLAND_DEVICE_IN_RESOURCE:
      return META_ANLAND_INPUT_EVENT_NEEDS_RESOURCE_FDS;
    case ANLAND_DEVICE_IN_ACTION:
      break;
    default:
      g_warning ("Anland input ignored unknown event type %u", event->type);
      break;
    }

  return META_ANLAND_INPUT_EVENT_HANDLED;
}

gboolean
meta_anland_input_inject_text (MetaAnlandInput *input,
                               const char      *text)
{
  const char *cursor = text;
  gboolean injected = FALSE;

  if (!input || !input->keyboard || !text)
    return FALSE;

  while (*cursor)
    {
      gunichar keyval;

      keyval = g_utf8_get_char_validated (cursor, -1);
      if (keyval == (gunichar) -1 || keyval == (gunichar) -2)
        return FALSE;

      switch (keyval)
        {
        case '\n':
        case '\r':
          keyval = CLUTTER_KEY_Return;
          break;
        case '\t':
          keyval = CLUTTER_KEY_Tab;
          break;
        case '\b':
          keyval = CLUTTER_KEY_BackSpace;
          break;
        default:
          break;
        }

      clutter_virtual_input_device_notify_keyval (
        input->keyboard, g_get_monotonic_time (), keyval,
        CLUTTER_KEY_STATE_PRESSED);
      clutter_virtual_input_device_notify_keyval (
        input->keyboard, g_get_monotonic_time (), keyval,
        CLUTTER_KEY_STATE_RELEASED);
      cursor = g_utf8_next_char (cursor);
      injected = TRUE;
    }

  return injected;
}

void
meta_anland_input_reset (MetaAnlandInput *input)
{
  guint code;

  if (!input)
    return;

  if (input->touch_batch_timeout_id)
    {
      g_source_remove (input->touch_batch_timeout_id);
      input->touch_batch_timeout_id = 0;
    }

  for (code = 0; code < G_N_ELEMENTS (input->pressed_keys); code++)
    {
      if (!input->pressed_keys[code])
        continue;

      clutter_virtual_input_device_notify_key (
        input->keyboard, g_get_monotonic_time (), code,
        CLUTTER_KEY_STATE_RELEASED);
      input->pressed_keys[code] = FALSE;
    }

  for (code = BTN_LEFT; code < G_N_ELEMENTS (input->pressed_buttons); code++)
    {
      if (!input->pressed_buttons[code])
        continue;

      clutter_virtual_input_device_notify_button (
        input->pointer, g_get_monotonic_time (),
        meta_evdev_button_to_clutter (code), CLUTTER_BUTTON_STATE_RELEASED);
      input->pressed_buttons[code] = FALSE;
    }

  cancel_touches (input);
  g_array_set_size (input->touch_batch, 0);
}