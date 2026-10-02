/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-anland-clipboard.h"

#include "meta/display.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-selection.h"
#include "meta/meta-selection-source-memory.h"

#include <string.h>

#define ANLAND_CLIPBOARD_LIMIT (1024 * 1024)

typedef struct
{
  MetaAnlandClipboard *clipboard;
  guint generation;
  GOutputStream *output;
} ClipboardTransfer;

struct _MetaAnlandClipboard
{
  int ref_count;
  MetaSelection *selection;
  MetaSelectionSource *source;
  anland_device *device;
  GCancellable *transfer_cancellable;
  GBytes *cached_contents;
  guint generation;
  gulong owner_changed_id;
  gboolean disposed;
};

static MetaAnlandClipboard *
meta_anland_clipboard_ref (MetaAnlandClipboard *clipboard)
{
  clipboard->ref_count++;
  return clipboard;
}

static void
meta_anland_clipboard_unref (MetaAnlandClipboard *clipboard)
{
  if (--clipboard->ref_count != 0)
    return;

  g_clear_object (&clipboard->transfer_cancellable);
  g_clear_object (&clipboard->source);
  g_clear_pointer (&clipboard->cached_contents, g_bytes_unref);
  g_clear_object (&clipboard->selection);
  g_free (clipboard);
}

static gboolean
contents_are_valid (GBytes *contents)
{
  const char *data;
  gsize size;

  data = g_bytes_get_data (contents, &size);
  if (size == 0)
    return TRUE;

  return size <= ANLAND_CLIPBOARD_LIMIT &&
         !memchr (data, '\0', size) &&
         g_utf8_validate (data, size, NULL);
}

static gboolean
contents_equal (GBytes *a,
                GBytes *b)
{
  return a && b && g_bytes_equal (a, b);
}

static void
send_contents (MetaAnlandClipboard *clipboard,
               GBytes              *contents)
{
  const void *data;
  gsize size;

  if (!clipboard->device || !anland_device_is_connected (clipboard->device))
    return;

  data = g_bytes_get_data (contents, &size);
  anland_device_set_clipboard (clipboard->device, data, size);
}

static void
set_cached_contents (MetaAnlandClipboard *clipboard,
                     GBytes              *contents)
{
  if (contents_equal (clipboard->cached_contents, contents))
    return;

  g_clear_pointer (&clipboard->cached_contents, g_bytes_unref);
  clipboard->cached_contents = g_bytes_ref (contents);
}

static void
clipboard_transfer_free (ClipboardTransfer *transfer)
{
  g_clear_object (&transfer->output);
  meta_anland_clipboard_unref (transfer->clipboard);
  g_free (transfer);
}

static void
on_selection_transfer_complete (MetaSelection *selection,
                                GAsyncResult  *result,
                                gpointer       user_data)
{
  ClipboardTransfer *transfer = user_data;
  MetaAnlandClipboard *clipboard = transfer->clipboard;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) contents = NULL;

  if (!meta_selection_transfer_finish (selection, result, &error))
    {
      if (!error || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Anland failed to transfer clipboard: %s",
                   error ? error->message : "unknown error");
      goto out;
    }

  if (clipboard->disposed || transfer->generation != clipboard->generation)
    goto out;

  g_output_stream_close (transfer->output, NULL, NULL);
  contents = g_memory_output_stream_steal_as_bytes (
    G_MEMORY_OUTPUT_STREAM (transfer->output));
  if (!contents_are_valid (contents))
    {
      g_warning ("Anland ignored invalid or oversized clipboard contents");
      goto out;
    }

  set_cached_contents (clipboard, contents);
  send_contents (clipboard, contents);

out:
  clipboard_transfer_free (transfer);
}

static const char *
choose_mimetype (MetaSelection *selection)
{
  GList *mimetypes;
  GList *l;
  const char *chosen = NULL;

  mimetypes = meta_selection_get_mimetypes (selection,
                                            META_SELECTION_CLIPBOARD);
  for (l = mimetypes; l; l = l->next)
    {
      if (g_str_equal (l->data, "text/plain;charset=utf-8"))
        {
          chosen = "text/plain;charset=utf-8";
          break;
        }

      if (!chosen && g_str_equal (l->data, "text/plain"))
        chosen = "text/plain";
    }
  g_list_free_full (mimetypes, g_free);

  return chosen;
}

static void
cancel_selection_transfer (MetaAnlandClipboard *clipboard)
{
  if (clipboard->transfer_cancellable)
    g_cancellable_cancel (clipboard->transfer_cancellable);
  g_clear_object (&clipboard->transfer_cancellable);
}

static void
on_selection_owner_changed (MetaSelection       *selection,
                            MetaSelectionType    selection_type,
                            MetaSelectionSource *owner,
                            MetaAnlandClipboard *clipboard)
{
  ClipboardTransfer *transfer;
  const char *mimetype;
  GOutputStream *output;

  if (selection_type != META_SELECTION_CLIPBOARD || clipboard->disposed)
    return;

  cancel_selection_transfer (clipboard);
  clipboard->generation++;

  if (owner == clipboard->source)
    return;

  g_clear_object (&clipboard->source);
  if (!owner)
    {
      g_autoptr (GBytes) empty = g_bytes_new_static ("", 0);

      set_cached_contents (clipboard, empty);
      send_contents (clipboard, empty);
      return;
    }

  mimetype = choose_mimetype (selection);
  if (!mimetype)
    return;

  output = g_memory_output_stream_new_resizable ();
  clipboard->transfer_cancellable = g_cancellable_new ();
  transfer = g_new0 (ClipboardTransfer, 1);
  transfer->clipboard = meta_anland_clipboard_ref (clipboard);
  transfer->generation = clipboard->generation;
  transfer->output = g_object_ref (output);
  meta_selection_transfer_async (selection, META_SELECTION_CLIPBOARD, mimetype,
                                 ANLAND_CLIPBOARD_LIMIT, output,
                                 clipboard->transfer_cancellable,
                                 (GAsyncReadyCallback) on_selection_transfer_complete,
                                 transfer);
  g_object_unref (output);
}

MetaAnlandClipboard *
meta_anland_clipboard_new (MetaBackend *backend)
{
  MetaAnlandClipboard *clipboard;
  MetaContext *context;
  MetaDisplay *display;

  context = meta_backend_get_context (backend);
  display = meta_context_get_display (context);
  if (!display)
    return NULL;

  clipboard = g_new0 (MetaAnlandClipboard, 1);
  clipboard->ref_count = 1;
  clipboard->selection = g_object_ref (meta_display_get_selection (display));
  clipboard->owner_changed_id = g_signal_connect_after (
    clipboard->selection, "owner-changed",
    G_CALLBACK (on_selection_owner_changed), clipboard);
  return clipboard;
}

void
meta_anland_clipboard_free (MetaAnlandClipboard *clipboard)
{
  if (!clipboard)
    return;

  clipboard->disposed = TRUE;
  meta_anland_clipboard_set_device (clipboard, NULL);
  if (clipboard->owner_changed_id)
    {
      g_signal_handler_disconnect (clipboard->selection,
                                   clipboard->owner_changed_id);
      clipboard->owner_changed_id = 0;
    }
  meta_anland_clipboard_unref (clipboard);
}

void
meta_anland_clipboard_set_device (MetaAnlandClipboard *clipboard,
                                  anland_device       *device)
{
  if (!clipboard || clipboard->device == device)
    return;

  clipboard->generation++;
  cancel_selection_transfer (clipboard);
  clipboard->device = device;

  /*
   * The consumer reads the Android clipboard as part of its reconnect path.
   * Do not race that initial sync by replaying cached compositor contents here:
   * when the app was in the background, doing so can overwrite a newer Android
   * clipboard before it has a chance to send it to Mutter.
   */
}

gboolean
meta_anland_clipboard_set_from_consumer (MetaAnlandClipboard *clipboard,
                                         GBytes              *contents)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (MetaSelectionSource) source = NULL;

  if (!clipboard || !contents_are_valid (contents))
    return FALSE;

  if (contents_equal (clipboard->cached_contents, contents))
    return TRUE;

  source = meta_selection_source_memory_new ("text/plain;charset=utf-8",
                                              contents, &error);
  if (!source)
    {
      g_warning ("Anland failed to create clipboard source: %s", error->message);
      return FALSE;
    }

  cancel_selection_transfer (clipboard);
  clipboard->generation++;
  set_cached_contents (clipboard, contents);
  g_set_object (&clipboard->source, source);
  meta_selection_set_owner (clipboard->selection, META_SELECTION_CLIPBOARD,
                            source);
  return TRUE;
}