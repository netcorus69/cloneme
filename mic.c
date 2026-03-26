#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include "vosk_api.h"
#include "clone.h"
#include "respond.h"
#include "voice.h"
#include "face.h"
#include "tee_log.h"

#define SAMPLE_RATE    16000
#define CHUNK_FRAMES   4096
#define CHUNK_BYTES    (CHUNK_FRAMES * sizeof(short))

#define WAKE_WORD_1  "hello"
#define WAKE_WORD_2  "ready"
#define WAKE_WORD_3  "listen"
#define WAKE_TIMEOUT 15

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
    if (!end || end == marker) return 0;
    size_t len = (size_t)(end - marker);
    if (len >= out_size) len = out_size - 1;
    strncpy(out, marker, len);
    out[len] = '\0';
    return 1;
}

// ─── HANDLE RECOGNIZED TEXT ──────────────────────────────────────────────────

static int handle_text(const char *text, VoskRecognizer *rec) {
    if (strstr(text, "exit")) {
        char *reply = respond("exit mic");
        fprintf(stderr, "CloneMe: %s\n", reply);
        speak(reply);
        animate_mouth_for(reply);
        return 0;
    }

    int is_wake = strstr(text, WAKE_WORD_1) != NULL ||
                  strstr(text, WAKE_WORD_2) != NULL ||
                  strstr(text, WAKE_WORD_3) != NULL;

    if (is_wake && !wake_active) {
        speak("Yes, I'm listening...");
        animate_mouth_for("Yes, I'm listening...");
        sleep(2);
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
            sleep(1);
            vosk_recognizer_reset(rec);
            fprintf(stderr, "🔇 Timed out\n");
        } else if (strlen(text) < 2) {
            // Too short — noise, keep listening
            wake_time = time(NULL);  // reset timeout
        } else {
            // Process command
            char *reply = respond(text);
            fprintf(stderr, "You:     %s\n", text);
            fprintf(stderr, "CloneMe: %s\n", reply);
            speak(reply);
            animate_mouth_for(reply);
            sleep(2);
            vosk_recognizer_reset(rec);
            wake_time = time(NULL);  // always reset timeout after response

            // Only sleep after a clearly executed command
            // Stay awake if asking for clarification or unknown command
            if (strstr(reply, "What should I do") ||
                strstr(reply, "don't recognize") ||
                strstr(reply, "don't know") ||
                strstr(reply, "Teach me") ||
                strstr(reply, "didn't catch")) {
                fprintf(stderr, "🎤 Waiting for your answer...\n");
            } else {
                wake_active = 0;
                fprintf(stderr, "🔇 Back to sleep\n");
            }
        }
    } else {
        fprintf(stderr, "💤 (not awake, heard: \"%s\")\n", text);
    }
    return 1;
}

void start_microphone(const char *model_path) {

    // Load Vosk model
    VoskModel *model = vosk_model_new(model_path);
    if (!model) {
        fprintf(stderr, "❌ Failed to load Vosk model: %s\n", model_path);
        return;
    }

    // Suppress Vosk verbose logging
    vosk_set_log_level(-1);

    VoskRecognizer *recognizer = vosk_recognizer_new(model, SAMPLE_RATE);
    if (!recognizer) {
        fprintf(stderr, "❌ Failed to create recognizer\n");
        vosk_model_free(model);
        return;
    }

    // Plain pulse at 16000Hz — proven to work by Python test
    FILE *pipe = popen("/usr/bin/arecord -D pulse -f S16_LE -r 16000 -c 1 -q 2>/dev/null", "r");
    if (!pipe) {
        fprintf(stderr, "❌ Failed to open arecord pipe\n");
        vosk_recognizer_free(recognizer);
        vosk_model_free(model);
        return;
    }

    // Debug: save first 5 seconds of audio to verify quality
    FILE *debug_wav = fopen("/tmp/cloneme_debug.wav", "wb");
    int debug_frames = 0;
    int max_debug_frames = (SAMPLE_RATE * 5) / CHUNK_FRAMES; // 5 seconds

    fprintf(stderr, "🎤 Listening... say '%s' or '%s' to activate\n",
            WAKE_WORD_1, WAKE_WORD_2);

    // ─── MAIN LOOP ────────────────────────────────────────────────────────────

    char buf[CHUNK_BYTES];
    int running = 1;

    while (running) {
        size_t bytes_read = fread(buf, 1, CHUNK_BYTES, pipe);
        if (bytes_read == 0) {
            fprintf(stderr, "⚠️  arecord pipe closed\n");
            break;
        }

        if (vosk_recognizer_accept_waveform(recognizer, buf, (int)bytes_read)) {
            const char *result = vosk_recognizer_result(recognizer);
            char text[256] = {0};
            if (parse_vosk_text(result, text, sizeof(text))) {
                normalize_text(text);
                fprintf(stderr, "🗣️  Heard: \"%s\"\n", text);
                running = handle_text(text, recognizer);
            }
        } else {
            // Show partial result in real time
            const char *partial = vosk_recognizer_partial_result(recognizer);
            const char *marker = strstr(partial, "\"partial\" : \"");
            if (marker) {
                marker += 13;
                const char *end = strchr(marker, '"');
                if (end && end > marker) {
                    char tmp[256] = {0};
                    size_t len = (size_t)(end - marker);
                    if (len > 0 && len < 255) {
                        strncpy(tmp, marker, len);
                        fprintf(stderr, "... %s\r", tmp);
                        fflush(stderr);
                    }
                }
            }
        }
    }

    // ─── CLEANUP ──────────────────────────────────────────────────────────────
    pclose(pipe);
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    fprintf(stderr, "🛑 Microphone stopped\n");
}
