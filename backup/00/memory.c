#include <stdio.h>
#include <stdlib.h>

void save_memory(const char *entry) {
    FILE *f = fopen("clone_memory.txt", "a");
    if (f) {
        fprintf(f, "%s\n", entry);
        fclose(f);
    }
}

void show_memory() {
    FILE *f = fopen("clone_memory.txt", "r");
    if (!f) {
        printf("No memory yet.\n");
        return;
    }
    char line[256];
    printf("Clone’s memory:\n");
    while (fgets(line, sizeof(line), f)) {
        printf("- %s", line);
    }
    fclose(f);
}
