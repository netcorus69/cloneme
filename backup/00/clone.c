#include <stdio.h>
#include <string.h>
#include "clone.h"
#include "memory.h"

void respond(const char *input) {
    if (strcmp(input, "hello") == 0) {
        printf("Hello, I’m your clone Muhammad!\n");
        speak("Hello, I’m your clone Muhammad!");
        save_memory("User said hello");
    } else if (strcmp(input, "how are you") == 0) {
        printf("I’m feeling curious and ready to learn.\n");
        save_memory("User asked how I am");
    } else {
        printf("That’s interesting. I’ll think about it.\n");
        save_memory("User said something new");
    }
}
