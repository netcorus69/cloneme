#include "voice.h"
#include "face.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tee_log.h"

#ifdef _WIN32
#include <windows.h>

void speak(const char *text) {
    if (!text || !*text) return;
    start_mouth_animation();
    char command[512];
    snprintf(command, sizeof(command),
        "PowerShell -Command \"Add-Type -AssemblyName System.Speech;"
        " $speak = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        " $speak.Speak('%s');\"",
        text);
    system(command);
    stop_mouth_animation();
}

#else
#include <unistd.h>
#include <sys/wait.h>

void speak(const char *text) {
    if (!text || !*text) return;

    start_mouth_animation();

    // Sanitize text: replace " with ' to avoid shell injection in the command
    char safe[512];
    int j = 0;
    for (int i = 0; text[i] && j < 510; i++) {
        safe[j++] = (text[i] == '"') ? '\'' : text[i];
    }
    safe[j] = '\0';

    // espeak --stdout writes raw PCM to stdout.
    // aplay -D pulse plays it via PulseAudio — avoids ALSA conflict entirely.
    char command[1024];
    snprintf(command, sizeof(command),
        "espeak -a 140 -s 140 -v en --stdout \"%s\" 2>/dev/null"
        " | /usr/bin/aplay -D pulse -f S16_LE -r 22050 -c 1 -q 2>/dev/null",
        safe);

    int ret = system(command);
    if (ret != 0) {
        tee_err("⚠️  speak() failed for: %s\n", safe);
    }

    stop_mouth_animation();
}

#endif
