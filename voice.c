#include "voice.h"
#include "face.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

    // Sanitize text
    char safe[512];
    int j = 0;
    for (int i = 0; text[i] && j < 510; i++) {
        safe[j++] = (text[i] == '"') ? '\'' : text[i];
    }
    safe[j] = '\0';

    // Step 1: Generate audio to temp file first (before animating)
    char gen_cmd[2048];
    snprintf(gen_cmd, sizeof(gen_cmd),
        "echo \"%s\" | piper --model /home/netcorus/cloneme_voices/en_US-amy-medium.onnx"
        " --output_file /tmp/cloneme_speech.wav 2>/dev/null",
        safe);

    int generated = (system(gen_cmd) == 0);

    // Step 2: Now start mouth animation and play simultaneously
    start_mouth_animation();

    if (generated) {
        system("aplay -q /tmp/cloneme_speech.wav 2>/dev/null");
    } else {
        // Fallback to espeak
        char esp_cmd[1024];
        snprintf(esp_cmd, sizeof(esp_cmd),
            "espeak -a 140 -s 140 -v en --stdout \"%s\" 2>/dev/null"
            " | paplay --raw --rate=22050 --channels=1 --format=s16le 2>/dev/null",
            safe);
        system(esp_cmd);
    }

    stop_mouth_animation();
}

#endif
