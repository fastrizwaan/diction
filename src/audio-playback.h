#pragma once

#include <glib.h>

typedef char *(*AudioResourceResolver)(const char *resource_dir,
                                       const char *sound_file,
                                       gpointer user_data);

void audio_play_file(const char *audio_path);
gboolean audio_try_play_encoded_sound_uri(const char *uri,
                                         AudioResourceResolver resolver,
                                         gpointer user_data);
