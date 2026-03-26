#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "memory.h"

#define MAX_MEMORY   500
#define MEMORY_FILE  "memory.txt"

static char *questions[MAX_MEMORY];
static char *answers[MAX_MEMORY];
static int   memory_count = 0;
static char  memory_path[2048] = {0};  // absolute path resolved at startup

// ─── RESOLVE ABSOLUTE PATH ───────────────────────────────────────────────────
// Called once at startup to set the absolute path to memory.txt
// next to the executable — survives working directory changes

static void resolve_memory_path() {
    if (memory_path[0] != '\0') return;  // already resolved

    char exe_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *last = strrchr(exe_path, '/');
        if (last) *last = '\0';
        snprintf(memory_path, sizeof(memory_path), "%s/%s", exe_path, MEMORY_FILE);
    } else {
        // fallback to home directory
        const char *home = getenv("HOME");
        if (home)
            snprintf(memory_path, sizeof(memory_path), "%s/cloneme_%s", home, MEMORY_FILE);
        else
            snprintf(memory_path, sizeof(memory_path), "/tmp/%s", MEMORY_FILE);
    }
}

// ─── LOAD ─────────────────────────────────────────────────────────────────────

void load_memory() {
    resolve_memory_path();

    FILE *f = fopen(memory_path, "r");
    if (!f) {
        memory_count = 0;
        return;
    }

    char line[512];
    memory_count = 0;
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline/carriage return
        line[strcspn(line, "\r\n")] = '\0';

        // Skip empty lines
        if (strlen(line) == 0) continue;

        // Find separator
        char *sep = strchr(line, '|');
        if (!sep) continue;

        *sep = '\0';
        char *keyword = line;
        char *answer  = sep + 1;

        // Skip blank keyword or answer
        if (strlen(keyword) == 0 || strlen(answer) == 0) continue;

        if (memory_count < MAX_MEMORY) {
            questions[memory_count] = strdup(keyword);
            answers[memory_count]   = strdup(answer);
            memory_count++;
        }
    }
    fclose(f);
    fprintf(stderr, "✅ Memory loaded: %d entries from %s\n", memory_count, memory_path);
}

// ─── PERSIST ──────────────────────────────────────────────────────────────────

void persist_memory() {
    resolve_memory_path();

    FILE *f = fopen(memory_path, "w");
    if (!f) {
        fprintf(stderr, "❌ Cannot write memory file: %s\n", memory_path);
        return;
    }

    for (int i = 0; i < memory_count; i++)
        fprintf(f, "%s|%s\n", questions[i], answers[i]);

    fclose(f);
    fprintf(stderr, "💾 Memory saved: %d entries to %s\n", memory_count, memory_path);
}

// ─── SAVE (append + update in-memory) ────────────────────────────────────────

void save_memory(const char *keyword, const char *command) {
    resolve_memory_path();
    fprintf(stderr, "🔧 save_memory called: '%s' -> '%s'\n", keyword, command);

    if (!keyword || !command) return;
    if (strchr(keyword, '|') || strchr(command, '|')) return;

    // Update in-memory array
    for (int i = 0; i < memory_count; i++) {
        if (strcasecmp(questions[i], keyword) == 0) {
            free(answers[i]);
            answers[i] = strdup(command);
            persist_memory();  // rewrite file with updated value
            return;
        }
    }

    // New entry
    if (memory_count < MAX_MEMORY) {
        questions[memory_count] = strdup(keyword);
        answers[memory_count]   = strdup(command);
        memory_count++;
        persist_memory();
    } else {
        fprintf(stderr, "⚠️  Memory full (%d entries) — cannot save '%s'\n", MAX_MEMORY, keyword);
    }
}

// ─── RECALL ───────────────────────────────────────────────────────────────────

const char* recall_memory(const char *keyword) {
    for (int i = 0; i < memory_count; i++) {
        if (strcasecmp(questions[i], keyword) == 0)
            return answers[i];
    }
    return NULL;
}

// ─── FORGET ───────────────────────────────────────────────────────────────────

void forget_memory(const char *question) {
    for (int i = 0; i < memory_count; i++) {
        if (strcasecmp(questions[i], question) == 0) {
            free(questions[i]);
            free(answers[i]);
            for (int j = i; j < memory_count - 1; j++) {
                questions[j] = questions[j+1];
                answers[j]   = answers[j+1];
            }
            memory_count--;
            persist_memory();
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
            answers[j]   = answers[j+1];
        }
        memory_count--;
        persist_memory();
    }
}

// ─── ACCESSORS ────────────────────────────────────────────────────────────────

int get_memory_count(void) { return memory_count; }

const char *get_memory_key(int index) {
    return (index >= 0 && index < memory_count) ? questions[index] : NULL;
}

const char *get_memory_value(int index) {
    return (index >= 0 && index < memory_count) ? answers[index] : NULL;
}

void add_memory(const char *item) {
    char question[256], answer[256];
    const char *sep = strchr(item, ' ');
    if (sep) {
        size_t qlen = sep - item;
        strncpy(question, item, qlen);
        question[qlen] = '\0';
        strncpy(answer, sep+1, sizeof(answer)-1);
        answer[sizeof(answer)-1] = '\0';
        save_memory(question, answer);
    } else {
        save_memory(item, item);
    }
}

char* list_memory_as_string() {
    static char buffer[2048];
    buffer[0] = '\0';

    if (memory_count == 0) {
        snprintf(buffer, sizeof(buffer), "Memory is empty.");
        return buffer;
    }

    snprintf(buffer, sizeof(buffer), "Memory list:\n");
    for (int i = 0; i < memory_count; i++) {
        strncat(buffer, "- ", sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, questions[i], sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, " -> ", sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, answers[i], sizeof(buffer) - strlen(buffer) - 1);
        strncat(buffer, "\n", sizeof(buffer) - strlen(buffer) - 1);
    }
    return buffer;
}
