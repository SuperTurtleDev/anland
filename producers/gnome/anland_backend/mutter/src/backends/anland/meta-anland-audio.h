/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

typedef struct _MetaAnlandAudio MetaAnlandAudio;

MetaAnlandAudio * meta_anland_audio_new (void);
void meta_anland_audio_free (MetaAnlandAudio *audio);

void meta_anland_audio_set_fd (MetaAnlandAudio *audio,
                               int              fd);
