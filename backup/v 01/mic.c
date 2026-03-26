#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <vosk_api.h>
#include <portaudio.h>
#include "clone.h"
#include "respond.h"
#include "voice.h"
#include "face.h"
#include "tee_log.h"

#define SAMPLE_RATE       16000
#define FRAMES_PER_BUFFER 320   // original working value

#define WAKE_WORD_1  "hello"
#define WAKE_WORD_2  "computer"
#define WAKE_TIMEOUT 8

static VoskModel      *model      = NULL;
static VoskRecognizer *recognizer = NULL;
static PaStream       *stream     = NULL;

static int    wake_active = 0;
static time_t wake_time   = 0;

// ─── TEXT NORMALIZATION ───────────────────────────────────────────────────────

static void normalize_text(char *text) {
    int start = 0;
    while (text[start] == ' ') start++;
    if (start > 0) memmove(text, text + start, strlen(text) - start + 1);
    int len = strlen(text);
    while (len > 0 && text[len-1] == ' ') text[--len] = '\0';
    for (int i = 0; text[i]; i++)
        text[i] = tolower((unsigned char)text[i]);
}

// ─── PARSE VOSK JSON ─────────────────────────────────────────────────────────

static int parse_vosk_text(const char *json, char *out, size_t out_size) {
    const char *marker = strstr(json, "\"text\" : \"");
    if (!marker) return 0;
    marker += 10;
    const char *end = strchr(marker, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - marker);
    if (len == 0) return 0;
    if (len >= out_size) len = out_size - 1;
    strncpy(out, marker, len);
    out[len] = '\0';
    return 1;
}

// ─── HANDLE RECOGNIZED TEXT ──────────────────────────────────────────────────
// Returns 1 to keep running, 0 to exit

// Drain mic buffer — read and discard audio captured while speaker was playing
static void drain_mic(PaStream *s, int drain_ms) {
    short discard[FRAMES_PER_BUFFER];
    int iterations = (drain_ms * SAMPLE_RATE) / (FRAMES_PER_BUFFER * 1000) + 1;
    for (int i = 0; i < iterations; i++) {
        Pa_ReadStream(s, discard, FRAMES_PER_BUFFER);
    }
}

static int handle_text(const char *text, VoskRecognizer *rec, PaStream *s) {
    if (strstr(text, "exit") || strstr(text, "quit")) {
        char *reply = respond("exit mic");
        fprintf(stderr, "CloneMe: %s\n", reply);
        speak(reply);
        animate_mouth_for(reply);
        return 0;
    }

    int is_wake = strstr(text, WAKE_WORD_1) != NULL ||
                  strstr(text, WAKE_WORD_2) != NULL;

    if (is_wake && !wake_active) {
        speak("Yes, I'm listening...");
        animate_mouth_for("Yes, I'm listening...");
        drain_mic(s, 2000);            // discard 2s of echo
        vosk_recognizer_reset(rec);
        wake_time   = time(NULL);
        wake_active = 1;
        fprintf(stderr, "🟢 Activated — say your command\n");
        return 1;
    }

    if (wake_active) {
        if (difftime(time(NULL), wake_time) > WAKE_TIMEOUT) {
            wake_active = 0;
            speak("Going back to sleep.");
            animate_mouth_for("Going back to sleep.");
            drain_mic(s, 2000);
            vosk_recognizer_reset(rec);
            fprintf(stderr, "🔇 Timed out\n");
        } else {
            char *reply = respond(text);
            fprintf(stderr, "You:     %s\n", text);
            fprintf(stderr, "CloneMe: %s\n", reply);
            speak(reply);
            animate_mouth_for(reply);
            drain_mic(s, 2000);        // discard echo of the reply
            vosk_recognizer_reset(rec);
            wake_active = 0;
            fprintf(stderr, "🔇 Back to sleep\n");
        }
    } else {
        fprintf(stderr, "💤 (not awake, heard: \"%s\")\n", text);
    }
    return 1;
}

// ─── MAIN ENTRY POINT ─────────────────────────────────────────────────────────

void start_microphone(const char *model_path) {

    // Load Vosk model
    model = vosk_model_new(model_path);
    if (!model) {
        fprintf(stderr, "❌ Failed to load Vosk model at %s\n", model_path);
        return;
    }

    recognizer = vosk_recognizer_new(model, SAMPLE_RATE);
    if (!recognizer) {
        fprintf(stderr, "❌ Failed to create recognizer\n");
        vosk_model_free(model);
        return;
    }

    // Initialize PortAudio — exactly as original working code
    if (Pa_Initialize() != paNoError) {
        fprintf(stderr, "❌ Failed to initialize PortAudio\n");
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return;
    }

    // Open default stream — blocking poll mode, exactly as original
    if (Pa_OpenDefaultStream(&stream,
                             1, 0,
                             paInt16,
                             SAMPLE_RATE,
                             FRAMES_PER_BUFFER,
                             NULL, NULL) != paNoError) {
        fprintf(stderr, "❌ Failed to open mic stream\n");
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return;
    }

    if (Pa_StartStream(stream) != paNoError) {
        fprintf(stderr, "❌ Failed to start mic stream\n");
        Pa_CloseStream(stream);
        Pa_Terminate();
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return;
    }

    fprintf(stderr, "🎤 Mic opened — say '%s' or '%s' to activate\n",
            WAKE_WORD_1, WAKE_WORD_2);

    // ─── POLL LOOP (original working approach) ────────────────────────────────

    short buffer[FRAMES_PER_BUFFER];
    int running = 1;

    while (running && Pa_IsStreamActive(stream) == 1) {
        Pa_ReadStream(stream, buffer, FRAMES_PER_BUFFER);

        if (vosk_recognizer_accept_waveform(recognizer,
                                            (const char *)buffer,
                                            sizeof(buffer))) {
            const char *result = vosk_recognizer_result(recognizer);
            char text[256] = {0};
            if (parse_vosk_text(result, text, sizeof(text))) {
                normalize_text(text);
                fprintf(stderr, "🗣️  Heard: \"%s\"\n", text);
                running = handle_text(text, recognizer, stream);
            }
        }
    }

    // ─── CLEANUP ──────────────────────────────────────────────────────────────
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    stream     = NULL;
    recognizer = NULL;
    model      = NULL;
    fprintf(stderr, "🛑 Microphone stopped\n");
}
