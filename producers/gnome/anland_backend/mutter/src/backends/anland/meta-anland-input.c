/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-anland-input.h"
#include "backends/anland/libdisplay_producer/anland_tablet.h"

#include "backends/meta-backend-private.h"
#if defined(ANLAND_MUTTER_51) || defined(ANLAND_MUTTER_50) || defined(ANLAND_MUTTER_49)
#include "backends/meta-logical-monitor-private.h"
#else
#include "backends/meta-logical-monitor.h"
#endif
#include "backends/meta-monitor-manager-private.h"
#include "backends/native/meta-input-thread.h"
#include "backends/native/meta-virtual-input-device-native.h"
#include "backends/native/meta-seat-native.h"
#include "clutter/clutter-event-private.h"

#include <linux/input.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANLAND_MAX_TOUCH_BATCH_EVENTS 128
#define ANLAND_TOUCH_BATCH_TIMEOUT_MS 100
#define ANLAND_MAX_PENDING_TABLET_FRAMES 256

struct _MetaAnlandInput
{
  MetaBackend *backend;
  ClutterSeat *seat;
  ClutterVirtualInputDevice *pointer;
  ClutterVirtualInputDevice *keyboard;
  ClutterVirtualInputDevice *touchscreen;
  ClutterVirtualInputDevice *tablet;
  ClutterInputDevice *tablet_device;
  GHashTable *tablet_tools;
  GPtrArray *tablet_tool_instances;
  GQueue pending_tablet_frames;
  gulong tablet_device_added_id;
  anland_tablet tablet_assembler;
  gboolean pressed_keys[KEY_CNT];
  gboolean pressed_buttons[KEY_CNT];
  GHashTable *touch_slots;
  gboolean active_touch_slots[CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS];
  GArray *touch_batch;
  guint touch_batch_timeout_id;

  MetaAnlandInputRefreshFunc refresh_func;
  gpointer refresh_user_data;
};

typedef struct _MetaAnlandTabletTool MetaAnlandTabletTool;
typedef struct _MetaAnlandTabletToolClass MetaAnlandTabletToolClass;

struct _MetaAnlandTabletTool
{
  ClutterInputDeviceTool parent_instance;
  uint32_t tool_id;
  uint32_t device_id;
  uint32_t tool_type;
  uint32_t capabilities;
};

struct _MetaAnlandTabletToolClass
{
  ClutterInputDeviceToolClass parent_class;
};

G_DEFINE_TYPE (MetaAnlandTabletTool, meta_anland_tablet_tool,
               CLUTTER_TYPE_INPUT_DEVICE_TOOL)

static void
meta_anland_tablet_tool_class_init (MetaAnlandTabletToolClass *klass)
{
  (void) klass;
}

static void
meta_anland_tablet_tool_init (MetaAnlandTabletTool *tool)
{
  (void) tool;
}

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

static ClutterInputDevice *
get_tablet_device (MetaAnlandInput *input)
{
  GList *devices, *l;

  if (input->tablet_device)
    return input->tablet_device;

  devices = clutter_seat_list_devices (input->seat);
  for (l = devices; l; l = l->next)
    {
      ClutterInputDevice *device = l->data;

      if (clutter_input_device_get_device_type (device) == CLUTTER_TABLET_DEVICE &&
          g_strcmp0 (clutter_input_device_get_device_name (device),
                     "Virtual device for seat") == 0)
        {
          input->tablet_device = g_object_ref (device);
          break;
        }
    }
  g_list_free (devices);

  return input->tablet_device;
}

typedef struct
{
  anland_tablet_sample previous;
  anland_tablet_sample sample;
  uint32_t input_width;
  uint32_t input_height;
} MetaAnlandTabletFrame;

typedef struct
{
  ClutterInputDevice *device;
  ClutterInputDeviceTool *tool;
  anland_tablet_sample previous;
  anland_tablet_sample sample;
  graphene_point_t coords;
} MetaAnlandTabletEmitFrame;

static void flush_tablet_frames (MetaAnlandInput *input);

static ClutterInputDeviceTool *
get_tablet_tool (MetaAnlandInput          *input,
                 const anland_tablet_sample *sample)
{
  MetaAnlandTabletTool *tool;
  gpointer key = GUINT_TO_POINTER (sample->tool_id);
  ClutterInputDeviceToolType clutter_type =
    sample->tool_type == TABLET_TOOL_ERASER ?
    CLUTTER_INPUT_DEVICE_TOOL_ERASER : CLUTTER_INPUT_DEVICE_TOOL_PEN;
  ClutterInputAxisFlags axes = 0;

  tool = g_hash_table_lookup (input->tablet_tools, key);
  if (tool && (tool->device_id != sample->device_id ||
               tool->tool_type != sample->tool_type ||
               tool->capabilities != sample->capabilities))
    tool = NULL;

  if (tool)
    return CLUTTER_INPUT_DEVICE_TOOL (tool);

  if (sample->capabilities & TABLET_CAP_PRESSURE)
    axes |= CLUTTER_INPUT_AXIS_FLAG_PRESSURE;
  if (sample->capabilities & TABLET_CAP_TILT)
    axes |= CLUTTER_INPUT_AXIS_FLAG_XTILT | CLUTTER_INPUT_AXIS_FLAG_YTILT;
  if (sample->capabilities & TABLET_CAP_DISTANCE)
    axes |= CLUTTER_INPUT_AXIS_FLAG_DISTANCE;

  tool = g_object_new (meta_anland_tablet_tool_get_type (),
                       "type", clutter_type,
                       "serial", (guint64) sample->tool_id,
                       "id", (guint64) sample->device_id,
                       "axes", axes,
                       NULL);
  tool->tool_id = sample->tool_id;
  tool->device_id = sample->device_id;
  tool->tool_type = sample->tool_type;
  tool->capabilities = sample->capabilities;
  g_ptr_array_add (input->tablet_tool_instances, tool);
  g_hash_table_insert (input->tablet_tools, key, tool);

  return CLUTTER_INPUT_DEVICE_TOOL (tool);
}

static void
put_tablet_event (ClutterEvent *event)
{
  if (!event)
    return;

  clutter_event_put (event);
  clutter_event_free (event);
}

static double *
tablet_axes (const anland_tablet_sample *sample)
{
  double *axes = g_new0 (double, CLUTTER_INPUT_AXIS_LAST);

  axes[CLUTTER_INPUT_AXIS_X] = sample->x;
  axes[CLUTTER_INPUT_AXIS_Y] = sample->y;
  axes[CLUTTER_INPUT_AXIS_PRESSURE] = sample->pressure;
  axes[CLUTTER_INPUT_AXIS_XTILT] = sample->tilt_x;
  axes[CLUTTER_INPUT_AXIS_YTILT] = sample->tilt_y;
  axes[CLUTTER_INPUT_AXIS_DISTANCE] = sample->distance;
  return axes;
}

static void
put_tablet_motion (ClutterInputDevice       *device,
                   ClutterInputDeviceTool   *tool,
                   const anland_tablet_sample *sample,
                   graphene_point_t           coords,
                   ClutterModifierType       state)
{
  ClutterEvent *event;

  event = clutter_event_motion_new (CLUTTER_EVENT_NONE,
                                   (int64_t) sample->time_msec * 1000,
                                   device, tool, state, coords,
                                   GRAPHENE_POINT_INIT (0, 0),
                                   GRAPHENE_POINT_INIT (0, 0),
                                   GRAPHENE_POINT_INIT (0, 0),
                                   tablet_axes (sample));
  put_tablet_event (event);
}

static void
put_tablet_proximity (ClutterInputDevice     *device,
                      ClutterInputDeviceTool *tool,
                      uint32_t                 time_msec,
                      gboolean                 in)
{
  put_tablet_event (clutter_event_proximity_new (
    in ? CLUTTER_PROXIMITY_IN : CLUTTER_PROXIMITY_OUT,
    CLUTTER_EVENT_NONE, (int64_t) time_msec * 1000, device, tool));
}

static void
put_tablet_button (ClutterInputDevice        *device,
                   ClutterInputDeviceTool    *tool,
                   const anland_tablet_sample *sample,
                   graphene_point_t            coords,
                   ClutterModifierType        state,
                   uint32_t                    evdev_button,
                   gboolean                    pressed)
{
  ClutterEvent *event;
  uint32_t clutter_button;

  if (evdev_button == BTN_TOUCH)
    clutter_button = CLUTTER_BUTTON_PRIMARY;
  else if (evdev_button == BTN_STYLUS)
    clutter_button = CLUTTER_BUTTON_MIDDLE;
  else
    clutter_button = CLUTTER_BUTTON_SECONDARY;

  event = clutter_event_button_new (pressed ? CLUTTER_BUTTON_PRESS : CLUTTER_BUTTON_RELEASE,
                                   CLUTTER_EVENT_NONE,
                                   (int64_t) sample->time_msec * 1000,
                                   device, tool, state, coords, clutter_button,
                                   evdev_button, tablet_axes (sample));
  put_tablet_event (event);
}

static gboolean
tablet_is_near (const anland_tablet_sample *sample)
{
  return (sample->flags & TABLET_FLAG_PROXIMITY) &&
    !(sample->flags & TABLET_FLAG_CANCELED);
}

static ClutterModifierType
tablet_button_state (const anland_tablet_sample *sample)
{
  ClutterModifierType state = 0;

  if (!tablet_is_near (sample))
    return 0;

  if (sample->flags & TABLET_FLAG_TIP_DOWN)
    state |= CLUTTER_BUTTON1_MASK;
  if (sample->buttons & TABLET_BUTTON_PRIMARY)
    state |= CLUTTER_BUTTON2_MASK;
  if (sample->buttons & TABLET_BUTTON_SECONDARY)
    state |= CLUTTER_BUTTON3_MASK;

  return state;
}

static gboolean
emit_tablet_frame_in_input_thread (GTask *task)
{
  MetaAnlandTabletEmitFrame *frame = g_task_get_task_data (task);
  ClutterSeat *seat = CLUTTER_SEAT (g_task_get_source_object (task));
  gboolean was_near = tablet_is_near (&frame->previous);
  gboolean is_near = tablet_is_near (&frame->sample);
  gboolean was_down = was_near &&
    (frame->previous.flags & TABLET_FLAG_TIP_DOWN);
  gboolean is_down = is_near &&
    (frame->sample.flags & TABLET_FLAG_TIP_DOWN);
  uint32_t new_buttons = is_near ? frame->sample.buttons : 0;
  ClutterModifierType state = 0;
  ClutterModifierType tablet_state = tablet_button_state (&frame->previous);

  /* Virtual keyboard notifications and pen frames are both GSources attached
   * to MetaSeatImpl's input context. Query here, after earlier key tasks have
   * run, so Shift/Ctrl/Alt and button-state snapshots follow wire order. */
  meta_seat_impl_query_state (META_SEAT_NATIVE (seat)->impl,
                              NULL, NULL, NULL, &state);

  if (!was_near && is_near)
    put_tablet_proximity (frame->device, frame->tool,
                          frame->sample.time_msec, TRUE);

  if (was_down && !is_down)
    {
      tablet_state &= ~CLUTTER_BUTTON1_MASK;
      put_tablet_button (frame->device, frame->tool, &frame->sample,
                         frame->coords, state | tablet_state,
                         BTN_TOUCH, FALSE);
    }
  if (is_near)
    put_tablet_motion (frame->device, frame->tool, &frame->sample,
                       frame->coords, state | tablet_state);
  if (!was_down && is_down)
    {
      tablet_state |= CLUTTER_BUTTON1_MASK;
      put_tablet_button (frame->device, frame->tool, &frame->sample,
                         frame->coords, state | tablet_state,
                         BTN_TOUCH, TRUE);
    }

  if ((frame->previous.buttons & TABLET_BUTTON_PRIMARY) !=
      (new_buttons & TABLET_BUTTON_PRIMARY))
    {
      if (new_buttons & TABLET_BUTTON_PRIMARY)
        tablet_state |= CLUTTER_BUTTON2_MASK;
      else
        tablet_state &= ~CLUTTER_BUTTON2_MASK;
      put_tablet_button (frame->device, frame->tool, &frame->sample,
                         frame->coords, state | tablet_state,
                         BTN_STYLUS,
                         (new_buttons & TABLET_BUTTON_PRIMARY) != 0);
    }
  if ((frame->previous.buttons & TABLET_BUTTON_SECONDARY) !=
      (new_buttons & TABLET_BUTTON_SECONDARY))
    {
      if (new_buttons & TABLET_BUTTON_SECONDARY)
        tablet_state |= CLUTTER_BUTTON3_MASK;
      else
        tablet_state &= ~CLUTTER_BUTTON3_MASK;
      put_tablet_button (frame->device, frame->tool, &frame->sample,
                         frame->coords, state | tablet_state,
                         BTN_STYLUS2,
                         (new_buttons & TABLET_BUTTON_SECONDARY) != 0);
    }

  if (was_near && !is_near)
    put_tablet_proximity (frame->device, frame->tool,
                          frame->sample.time_msec, FALSE);

  g_task_return_boolean (task, TRUE);
  return G_SOURCE_REMOVE;
}

static void
free_tablet_emit_frame (gpointer data)
{
  MetaAnlandTabletEmitFrame *frame = data;

  g_clear_object (&frame->device);
  g_clear_object (&frame->tool);
  g_free (frame);
}

static void
emit_tablet_frame (MetaAnlandInput           *input,
                   const anland_tablet_sample *previous,
                   const anland_tablet_sample *sample,
                   uint32_t                    input_width,
                   uint32_t                    input_height)
{
  ClutterInputDevice *device = get_tablet_device (input);
  ClutterInputDeviceTool *tool;
  MetaAnlandTabletEmitFrame *frame;
  GTask *task;
  float x, y;

  if (!device)
    {
      g_warning ("Anland tablet device is not registered with Mutter's native seat");
      return;
    }

  tool = get_tablet_tool (input, sample);
  map_input_coordinates (input, input_width, input_height,
                         sample->x, sample->y, FALSE, &x, &y);
  frame = g_new0 (MetaAnlandTabletEmitFrame, 1);
  frame->device = g_object_ref (device);
  frame->tool = g_object_ref (tool);
  frame->previous = *previous;
  frame->sample = *sample;
  frame->coords = GRAPHENE_POINT_INIT (x, y);

  task = g_task_new (G_OBJECT (input->seat), NULL, NULL, NULL);
  g_task_set_task_data (task, frame, free_tablet_emit_frame);
  meta_seat_impl_run_input_task (META_SEAT_NATIVE (input->seat)->impl,
                                 task,
                                 (GSourceFunc) emit_tablet_frame_in_input_thread);
  g_object_unref (task);
}

static void
flush_tablet_frames (MetaAnlandInput *input)
{
  MetaAnlandTabletFrame *frame;

  if (!get_tablet_device (input))
    return;

  while ((frame = g_queue_pop_head (&input->pending_tablet_frames)))
    {
      emit_tablet_frame (input, &frame->previous, &frame->sample,
                         frame->input_width, frame->input_height);
      g_free (frame);
    }
}

static gboolean
handle_tablet_frame (MetaAnlandInput            *input,
                    const anland_tablet_sample *previous,
                    const anland_tablet_sample *sample,
                    uint32_t                    input_width,
                    uint32_t                    input_height)
{
  MetaAnlandTabletFrame *frame;

  if (!get_tablet_device (input))
    {
      /* Virtual devices are registered asynchronously by MetaSeatImpl. Keep
       * complete wire frames in order until the device-added signal lets the
       * native Wayland tablet seat see the source device. */
      if (g_queue_get_length (&input->pending_tablet_frames) >=
          ANLAND_MAX_PENDING_TABLET_FRAMES)
        {
          g_warning ("Anland tablet device registration stalled while frames accumulated");
          return FALSE;
        }
      frame = g_new (MetaAnlandTabletFrame, 1);
      frame->previous = *previous;
      frame->sample = *sample;
      frame->input_width = input_width;
      frame->input_height = input_height;
      g_queue_push_tail (&input->pending_tablet_frames, frame);
      return TRUE;
    }

  /* Preserve order if the input device became visible before its queued
   * CLUTTER_DEVICE_ADDED event was dispatched. */
  flush_tablet_frames (input);
  emit_tablet_frame (input, previous, sample, input_width, input_height);
  return TRUE;
}

static void
on_tablet_device_added (ClutterSeat        *seat,
                        ClutterInputDevice *device,
                        MetaAnlandInput    *input)
{
  (void) seat;

  if (clutter_input_device_get_device_type (device) != CLUTTER_TABLET_DEVICE ||
      g_strcmp0 (clutter_input_device_get_device_name (device),
                 "Virtual device for seat") != 0)
    return;

  if (!input->tablet_device)
    input->tablet_device = g_object_ref (device);
  flush_tablet_frames (input);
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
  input->seat = seat;
  input->touch_slots = g_hash_table_new (g_direct_hash, g_direct_equal);
  input->touch_batch = g_array_new (FALSE, FALSE, sizeof (MetaVirtualTouchEvent));
  input->tablet_tools = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_queue_init (&input->pending_tablet_frames);
  {
    GQuark tool_instances_quark =
      g_quark_from_static_string ("anland-tablet-tool-instances");
    input->tablet_tool_instances =
      g_object_get_qdata (G_OBJECT (seat), tool_instances_quark);
    if (!input->tablet_tool_instances)
      {
        input->tablet_tool_instances =
          g_ptr_array_new_with_free_func (g_object_unref);
        g_object_set_qdata_full (G_OBJECT (seat), tool_instances_quark,
                                 input->tablet_tool_instances,
                                 (GDestroyNotify) g_ptr_array_unref);
      }
  }
  input->pointer = clutter_seat_create_virtual_device (seat,
                                                        CLUTTER_POINTER_DEVICE);
  input->keyboard = clutter_seat_create_virtual_device (seat,
                                                         CLUTTER_KEYBOARD_DEVICE);
  input->touchscreen = clutter_seat_create_virtual_device (seat,
                                                            CLUTTER_TOUCHSCREEN_DEVICE);
  /* Mutter's public virtual-input helper omits a pen notification API, but the
   * native seat accepts a TABLET_DEVICE and routes Clutter tool events through
   * its regular Wayland tablet protocol path. */
  input->tablet = clutter_seat_create_virtual_device (seat,
                                                       CLUTTER_TABLET_DEVICE);
  if (!input->pointer || !input->keyboard || !input->touchscreen || !input->tablet)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create Anland virtual input devices");
      meta_anland_input_free (input);
      return NULL;
    }
  input->tablet_device_added_id =
    g_signal_connect (seat, "device-added",
                      G_CALLBACK (on_tablet_device_added), input);
  get_tablet_device (input);

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
  if (input->tablet_device_added_id)
    g_signal_handler_disconnect (input->seat, input->tablet_device_added_id);
  g_clear_object (&input->pointer);
  g_clear_object (&input->keyboard);
  g_clear_object (&input->touchscreen);
  g_clear_object (&input->tablet_device);
  g_clear_object (&input->tablet);
  g_clear_pointer (&input->touch_slots, g_hash_table_unref);
  g_clear_pointer (&input->touch_batch, g_array_unref);
  g_clear_pointer (&input->tablet_tools, g_hash_table_unref);
  g_queue_clear_full (&input->pending_tablet_frames, g_free);
  g_free (input);
}

MetaAnlandInputEventResult
meta_anland_input_handle_event (MetaAnlandInput             *input,
                                const anland_device_input_t *event,
                                uint32_t                     input_width,
                                uint32_t                     input_height)
{
  if (anland_tablet_is_event (event->type))
    {
      anland_tablet_sample previous = { 0 };
      anland_tablet_sample sample = { 0 };
      int result = anland_tablet_feed (&input->tablet_assembler, event,
                                       &previous, &sample);

      if (result < 0)
        return META_ANLAND_INPUT_EVENT_ERROR;
      if (result > 0)
        {
          if (!handle_tablet_frame (input, &previous, &sample,
                                    input_width, input_height))
            return META_ANLAND_INPUT_EVENT_ERROR;
        }
      return META_ANLAND_INPUT_EVENT_HANDLED;
    }

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
  unsigned int tablet_cursor = 0;
  anland_tablet_sample tablet_previous = { 0 };
  anland_tablet_sample tablet_sample = { 0 };

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

  if (get_tablet_device (input))
    flush_tablet_frames (input);
  else
    g_queue_clear_full (&input->pending_tablet_frames, g_free);

  while (anland_tablet_release_next (&input->tablet_assembler,
                                     &tablet_cursor,
                                     &tablet_previous,
                                     &tablet_sample))
    {
      /* No tablet event can have reached Mutter before source registration. */
      if (input->tablet_device)
        handle_tablet_frame (input, &tablet_previous, &tablet_sample, 0, 0);
    }
}
