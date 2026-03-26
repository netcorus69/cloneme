#include <stdio.h>
#include <string.h>
#include "clone.h"
#include "memory.h"
#include "voice.h"   // <-- add this include
#include "face.h"

void respondold(const char *input) {
    const char *answer = recall_memory(input);
    if (answer) {
        // Print and speak the stored answer
        printf("%s\n", answer);
        speak(answer);
    } else {
        char new_answer[256];
        printf("I don’t know how to respond. What should I say?\n> ");
        //animate_mouth_for("I don’t know how to respond. What should I say?");
        speak("I don’t know how to respond. What should I say?");
        if (fgets(new_answer, sizeof(new_answer), stdin)) {
            new_answer[strcspn(new_answer, "\n")] = 0;
            save_memory(input, new_answer);
            // Print and speak the new answer
            printf("%s\n", new_answer);
            speak(new_answer);
        }
    }
}
