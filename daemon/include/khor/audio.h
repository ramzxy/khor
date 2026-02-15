#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KhorAudio KhorAudio;

KhorAudio* khor_audio_create(void);
void khor_audio_destroy(KhorAudio* a);

// Returns 0 on failure, non-zero on success.
int khor_audio_start(KhorAudio* a);
void khor_audio_stop(KhorAudio* a);

// `velocity` in [0,1], `seconds` is note duration.
void khor_audio_note_on(KhorAudio* a, int midi, float velocity, double seconds);

#ifdef __cplusplus
}
#endif

