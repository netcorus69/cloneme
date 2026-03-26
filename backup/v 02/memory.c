#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"

#define MAX_MEMORY 100
#define MEMORY_FILE "memory.txt"

static char *questions[MAX_MEMORY];
static char *answers[MAX_MEMORY];
static int memory_count = 0;

void load_memory() {
    FILE *f = fopen(MEMORY_FILE, "r");
    if (!f) {
        memory_count = 0;
        return;
    }

    char q[256], a[256];
    memory_count = 0;
    while (fscanf(f, "%255[^|]|%255[^\n]\n", q, a) == 2) {
        if (memory_count < MAX_MEMORY) {
            questions[memory_count] = strdup(q);
            answers[memory_count] = strdup(a);
            memory_count++;
        }
    }
    fclose(f);
}

void persist_memory() {
    FILE *f = fopen(MEMORY_FILE, "w");
    if (!f) return;

    for (int i = 0; i < memory_count; i++) {
        fprintf(f, "%s|%s\n", questions[i], answers[i]);
    }
    fclose(f);
}

void save_memory(const char *keyword, const char *command) {
    FILE *f = fopen(MEMORY_FILE, "a");
    if (!f) return;
    if (strchr(keyword, '|') || strchr(command, '|')) {
        // prevent malformed entry
        fclose(f);
        return;
    }
    fprintf(f, "%s|%s\n", keyword, command);
    fclose(f);
}


const char* recall_memory(const char *keyword) {
    static char line[512];
    static char answer[256];
    FILE *f = fopen(MEMORY_FILE, "r");
    if (!f) return NULL;

    while (fgets(line, sizeof(line), f)) {
        char *sep = strchr(line, '|');
        if (!sep) continue; // skip malformed lines
        *sep = '\0';
        char *stored_keyword = line;
        char *stored_answer = sep + 1;

        // trim newline and spaces
        stored_answer[strcspn(stored_answer, "\r\n")] = '\0';
        while (*stored_keyword == ' ') stored_keyword++;
        while (*stored_answer == ' ') stored_answer++;

        if (strcasecmp(stored_keyword, keyword) == 0) {
            strcpy(answer, stored_answer);
            fclose(f);
            return answer;
        }
    }
    fclose(f);
    return NULL;
}


void forget_memory(const char *question) {
    for (int i = 0; i < memory_count; i++) {
        if (strcasecmp(questions[i], question) == 0) {
            free(questions[i]);
            free(answers[i]);
            for (int j = i; j < memory_count - 1; j++) {
                questions[j] = questions[j+1];
                answers[j] = answers[j+1];
            }
            memory_count--;
            persist_memory(); // update file
            return;
        }
    }
}

void forget_memory_by_index(int index) {
    if (index >= 0 && index < memory_count) {
        free(questions[index]);
        free(answers[index]);
        for (int j = index; j < memory_count - 1; j++) {
            questions[j] = questions[j+1];
            answers[j] = answers[j+1];
        }
        memory_count--;
        persist_memory(); // update file
    }
}

// Simple wrapper so respond.c can call add_memory()
void add_memory(const char *item) {
    // Split into "question answer" if possible
    char question[256], answer[256];
    const char *sep = strchr(item, ' ');
    if (sep) {
        size_t qlen = sep - item;
        strncpy(question, item, qlen);
        question[qlen] = '\0';
        strncpy(answer, sep + 1, sizeof(answer)-1);
        answer[sizeof(answer)-1] = '\0';
        save_memory(question, answer);
    } else {
        // If no answer provided, store item as both question and answer
        save_memory(item, item);
    }
}


// === Accessors used by respond.c ===
int get_memory_count(void) {
    return memory_count;
}

const char *get_memory_key(int index) {
    if (index >= 0 && index < memory_count) {
        return questions[index];
    }
    return NULL;
}

const char *get_memory_value(int index) {
    if (index >= 0 && index < memory_count) {
        return answers[index];
    }
    return NULL;
}

char* list_memory_as_string() {
    static char buffer[2048];
    buffer[0] = '\0';

    if (memory_count == 0) {
        snprintf(buffer, sizeof(buffer), "📂 Memory is empty.");
        return buffer;
    }

    snprintf(buffer, sizeof(buffer), "📂 Memory list:\n");
    for (int i = 0; i < memory_count; i++) {
        strncat(buffer, "- ", sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, questions[i], sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, " -> ", sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, answers[i], sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
    }
    return buffer;
}
