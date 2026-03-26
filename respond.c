#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "respond.h"
#include "voice.h"
#include "memory.h"
#include "face.h"
#include <unistd.h>
#include <sys/stat.h>

extern int mic_mode;

static int  learning_mode   = 0;
static char pending_keyword[128];

// ─── VOCABULARY LOOKUP ───────────────────────────────────────────────────────
// Check if a word/phrase exists in vocabulary.json.
// Returns 1 if found, 0 if not. Used to give better "I know that word" replies.

static int in_vocabulary(const char *text) {
    FILE *f = fopen("vocabulary.json", "r");
    if (!f) return 0;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        // Each entry looks like: "word" or "word",
        // Search for the word surrounded by quotes
        char needle[256];
        snprintf(needle, sizeof(needle), "\"%s\"", text);
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

// ─── MAIN RESPOND FUNCTION ───────────────────────────────────────────────────

char* respond(const char *text) {
    static char buffer[512];

    if (text == NULL || strlen(text) == 0) {
        return "I didn't catch that.";
    }

    // ── Learning mode: waiting for user to teach a command ───────────────────
    if (learning_mode) {
        if (strncmp(text, "system:", 7) == 0) {
            save_memory(pending_keyword, text);
            learning_mode = 0;
            snprintf(buffer, sizeof(buffer),
                     "✅ Learned system command for '%s' -> %s",
                     pending_keyword, text + 7);
            return buffer;
        } else {
            save_memory(pending_keyword, text);
            learning_mode = 0;
            snprintf(buffer, sizeof(buffer),
                     "✅ Learned reply for '%s' -> %s",
                     pending_keyword, text);
            return buffer;
        }
    }

    // Quit — set mode to quit so main loop handles cleanup properly
    if (strcasecmp(text, "quit") == 0 ||
        strcasecmp(text, "exit program") == 0) {
        extern int mic_mode;
        mic_mode = -1;  // MODE_QUIT
        return "Goodbye!";
    }

    // ── Mode switching ────────────────────────────────────────────────────────
    if (strcasecmp(text, "mic") == 0) {
        mic_mode = 1;
        return "🎤 Switched to microphone mode.";
    }
    if (strcasecmp(text, "text") == 0 ||
        strcasecmp(text, "exit mic") == 0) {
        mic_mode = 0;
        return "⌨️ Switched back to text mode.";
    }

    // ── Memory commands ───────────────────────────────────────────────────────
    if (strcasecmp(text, "memory") == 0 ||
        strcasecmp(text, "memory list") == 0) {
        return list_memory_as_string();
    }
    if (strncasecmp(text, "remember ", 9) == 0) {
        char keyword[128], answer[256];
        const char *rest = text + 9;
        const char *sep = strchr(rest, '|');
        if (!sep) sep = strchr(rest, ' ');
        if (sep) {
            size_t klen = sep - rest;
            strncpy(keyword, rest, klen);
            keyword[klen] = '\0';
            strncpy(answer, sep + 1, sizeof(answer)-1);
            answer[sizeof(answer)-1] = '\0';
            save_memory(keyword, answer);
            snprintf(buffer, sizeof(buffer),
                     "✅ New command remembered: %s -> %s", keyword, answer);
            return buffer;
        } else {
            return "⚠️ Usage: remember <keyword>|<answer>";
        }
    }
    if (strncasecmp(text, "forget ", 7) == 0) {
        forget_memory(text + 7);
        snprintf(buffer, sizeof(buffer), "🗑️ Forgotten: %s", text + 7);
        return buffer;
    }

    // ── Open file ─────────────────────────────────────────────────────────────
    if (strncasecmp(text, "open ", 5) == 0) {
        const char *filename = text + 5;
        char fullpath[512];
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(fullpath, sizeof(fullpath), "%s/Documents/cloneme/%s", home, filename);

        char folder[512];
        snprintf(folder, sizeof(folder), "%s/Documents/cloneme", home);
        mkdir(folder, 0755);

        if (access(fullpath, F_OK) != 0) {
            FILE *f = fopen(fullpath, "w");
            if (f) fclose(f);
        }

        const char *ext = strrchr(filename, '.');
        char cmd[1024];
        if (ext && strcasecmp(ext, ".odt") == 0)
            snprintf(cmd, sizeof(cmd), "libreoffice --writer '%s' &", fullpath);
        else if (ext && strcasecmp(ext, ".ods") == 0)
            snprintf(cmd, sizeof(cmd), "libreoffice --calc '%s' &", fullpath);
        else if (ext && strcasecmp(ext, ".odp") == 0)
            snprintf(cmd, sizeof(cmd), "libreoffice --impress '%s' &", fullpath);
        else
            snprintf(cmd, sizeof(cmd), "xdg-open '%s' &", fullpath);

        system(cmd);
        snprintf(buffer, sizeof(buffer), "📂 Opening file: %s", fullpath);
        return buffer;
    }

    // ── Save file ─────────────────────────────────────────────────────────────
    if (strncasecmp(text, "save ", 5) == 0) {
        const char *rest = text + 5;
        if (!rest || rest[0] == '\0') return "⚠️ No file name provided to save.";

        const char *sep = strchr(rest, '|');
        char filename[256], content[512];

        if (sep) {
            size_t len = sep - rest;
            if (len >= sizeof(filename)) len = sizeof(filename) - 1;
            strncpy(filename, rest, len);
            filename[len] = '\0';
            snprintf(content, sizeof(content), "%s", sep + 1);
        } else {
            snprintf(filename, sizeof(filename), "%s", rest);
            snprintf(content, sizeof(content), "[Saved by CloneMe]");
        }

        char fullpath[1024];
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(fullpath, sizeof(fullpath), "%s/Documents/cloneme/%s", home, filename);

        char folder[1024];
        snprintf(folder, sizeof(folder), "%s/Documents/cloneme", home);
        mkdir(folder, 0755);

        FILE *f = fopen(fullpath, "a");
        if (!f) {
            snprintf(buffer, sizeof(buffer), "❌ Failed to save file: %s", fullpath);
            return buffer;
        }
        fprintf(f, "\n%s\n", content);
        fclose(f);

        snprintf(buffer, sizeof(buffer), "💾 Saved into file: %s", fullpath);
        return buffer;
    }

    // ── Run command ───────────────────────────────────────────────────────────
    if (strncasecmp(text, "run ", 4) == 0) {
        const char *target = text + 4;
        const char *ans = recall_memory(target);
        if (ans) {
            if (strncmp(ans, "system:", 7) == 0) {
                system(ans + 7);
                snprintf(buffer, sizeof(buffer), "⚙️ Running: %s", target);
                return buffer;
            }
            return ans;
        }
        strncpy(pending_keyword, target, sizeof(pending_keyword)-1);
        learning_mode = 1;
        snprintf(buffer, sizeof(buffer),
                 "I don't know how to run '%s'. Teach me — what command should I use?", target);
        return buffer;
    }

    // ── Memory lookup ─────────────────────────────────────────────────────────
    const char *ans = recall_memory(text);
    if (ans) {
        if (strncmp(ans, "system:", 7) == 0) {
            system(ans + 7);
            return "⚙️ Executing system command...";
        }
        return ans;
    }

    // ── Vocabulary check → unknown command ────────────────────────────────────
    // Three tiers:
    // 1. Known word in vocabulary.json but no action yet → teach me
    // 2. Unknown word → ask what it means
    strncpy(pending_keyword, text, sizeof(pending_keyword)-1);
    pending_keyword[sizeof(pending_keyword)-1] = '\0';
    learning_mode = 1;

    if (in_vocabulary(text)) {
        // Word is recognized in vocabulary but has no stored action yet
        snprintf(buffer, sizeof(buffer),
                 "I know '%s' but don't have a command for it yet. What should I do?", text);
    } else {
        // Completely unknown — may be a new phrase or misrecognition
        snprintf(buffer, sizeof(buffer),
                 "I don't recognize '%s'. What should I do or say?", text);
    }
    return buffer;
}
