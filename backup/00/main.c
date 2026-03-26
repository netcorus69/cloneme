#include <stdio.h>
#include <string.h>
#include "clone.h"
#include "memory.h"
#include "voice.h"
#include "face.h"

int main() {
    start_face_thread(); // SDL face window runs in background

    char input[100];
    while (1) {
        printf("> ");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "quit") == 0) break;
        if (strcmp(input, "memory") == 0) {
            show_memory();
            continue;
        }

        respond(input);
        speak(input);              // async speech
        animate_mouth_for(input);  // animation duration matches speech
    }

    stop_face_thread();
    return 0;
}
