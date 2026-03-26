#include "voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

void speak(const char *text) {
    // Build PowerShell command string
    char command[512];
    snprintf(command, sizeof(command),
        "PowerShell -Command \"Add-Type –AssemblyName System.Speech;"
        " $speak = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
        " $speak.Speak('%s');\"",
        text);

    // Launch asynchronously (non-blocking)
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);

    if (CreateProcess(NULL, command, NULL, NULL, FALSE,
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf("Failed to start speech process.\n");
    }
}

#else // Linux / macOS

#include <unistd.h>

void speak(const char *text) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: run espeak
        execlp("espeak", "espeak", text, NULL);
        exit(0);
    }
    // Parent returns immediately (non-blocking)
}

#endif
