#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <semaphore.h>
#include <time.h>
#include <fcntl.h>
#define TEE_LOG_DEFINE
#include "tee_log.h"
#include <locale.h>

#define PATH_SEPARATOR        '/'
#define PATH_MAX_LEN          2048
#define VOSK_MODEL            "vosk-model-small-en-in-0.4"
#define FACE_READY_TIMEOUT_MS 10000

#define MODE_TEXT  0
#define MODE_MIC   1
#define MODE_QUIT -1

void load_memory();
void persist_memory();
void start_face_thread();
void stop_face_thread();
void speak(const char *text);
char* respond(const char *text);
void animate_mouth_for(const char *input);
void start_microphone(const char *model);

extern sem_t face_ready_sem;

int mic_mode = MODE_TEXT;  // start in text mode — type 'mic' to switch to voice

// GTK globals
GtkWidget    *window   = NULL;
GtkWidget    *entry    = NULL;
GtkWidget    *textview = NULL;
GtkTextBuffer *buffer  = NULL;

// ─── APPEND TEXT ─────────────────────────────────────────────────────────────

void append_text(const char *prefix, const char *text) {
    if (!buffer) return;  // window not ready
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, prefix, -1);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(textview), mark, 0.0, TRUE, 0.0, 1.0);
}

// ─── ON ENTER ────────────────────────────────────────────────────────────────

void on_enter(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;

    const gchar *input_raw = gtk_entry_get_text(GTK_ENTRY(entry));
    char input[256];
    strncpy(input, input_raw, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    if (strlen(input) == 0) return;

    gtk_entry_set_text(GTK_ENTRY(entry), "");
    append_text("You: ", input);

    char *reply = respond(input);
    if (reply && strlen(reply) > 0) {
        append_text("CloneMe: ", reply);
        speak(reply);
        animate_mouth_for(reply);
    } else {
        append_text("CloneMe: ", "...");
    }

    if (strcasecmp(input, "mic") == 0) {
        mic_mode = MODE_MIC;
        gtk_widget_destroy(window);
        gtk_main_quit();
    }

    if (strcasecmp(input, "quit") == 0 ||
        strcasecmp(input, "exit program") == 0) {
        mic_mode = MODE_QUIT;
        gtk_widget_destroy(window);
        gtk_main_quit();
    }
}

// ─── SHOW TEXT WINDOW ────────────────────────────────────────────────────────

void show_text_window() {
    // Flush pending GTK events
    while (gtk_events_pending())
        gtk_main_iteration();

    // Reset globals — previous window may be destroyed
    window   = NULL;
    entry    = NULL;
    textview = NULL;
    buffer   = NULL;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "CloneMe Text Mode");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 0);

    textview = gtk_text_view_new();
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), textview, TRUE, TRUE, 0);

    g_signal_connect(entry,  "activate", G_CALLBACK(on_enter),       NULL);
    g_signal_connect(window, "destroy",  G_CALLBACK(gtk_main_quit),   NULL);

    gtk_widget_show_all(window);
    gtk_main();

    // Null out after window is destroyed
    window   = NULL;
    entry    = NULL;
    textview = NULL;
    buffer   = NULL;
}

// ─── UTILITIES ───────────────────────────────────────────────────────────────

int ensure_folder(const char *path) {
    struct stat st = {0};
    if (!path || strlen(path) == 0) return -1;
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            fprintf(stderr, "❌ Failed to create folder '%s': %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void get_base_path(char *out, size_t out_size) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (len == 0) { strncpy(out, "C:\\cloneme", out_size); out[out_size-1]='\0'; return; }
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *last_sep = '\0';
    strncpy(out, exe_path, out_size - 1);
    out[out_size - 1] = '\0';
#else
    // Use /proc/self/exe to get the actual executable path
    char exe_path[2048] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        // Strip filename to get directory
        char *last_sep = strrchr(exe_path, '/');
        if (last_sep) *last_sep = '\0';
        strncpy(out, exe_path, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        // Fallback to current working directory
        if (!getcwd(out, out_size)) {
            strncpy(out, "/tmp", out_size - 1);
            out[out_size - 1] = '\0';
        }
    }
#endif
}

// ─── MAIN ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
	setlocale(LC_NUMERIC, "C");
    /* ── Path setup ── */
    char base_path[PATH_MAX_LEN];
    get_base_path(base_path, sizeof(base_path));
    // Project directory already exists — no need to create it

    /* ── Logging setup ── */
    char log_path[PATH_MAX_LEN], err_path[PATH_MAX_LEN];
    snprintf(log_path, sizeof(log_path), "%s%ccloneme_log.txt", base_path, PATH_SEPARATOR);
    snprintf(err_path, sizeof(err_path), "%s%ccloneme_err.txt", base_path, PATH_SEPARATOR);

    FILE *log_file = fopen(log_path, "a+");
    FILE *err_file = fopen(err_path, "a+");
    if (!log_file || !err_file) {
        fprintf(stderr, "❌ Failed to open log files in %s\n", base_path);
        return 1;
    }

    // FD_CLOEXEC only on log files — prevents duplicate log entries in children
    // Do NOT set on terminal_out/terminal_err — GTK needs these to stay open
    fcntl(fileno(log_file), F_SETFD, FD_CLOEXEC);
    fcntl(fileno(err_file), F_SETFD, FD_CLOEXEC);

    // Use original stdout/stderr for terminal — GTK needs these untouched
    setvbuf(stdout,   NULL, _IONBF, 0);
    setvbuf(stderr,   NULL, _IONBF, 0);
    setvbuf(log_file, NULL, _IONBF, 0);
    setvbuf(err_file, NULL, _IONBF, 0);

    tee_init(log_file, err_file, stdout, stderr);
    tee_out("✅ Logging initialized: stdout → %s, stderr → %s\n", log_path, err_path);

    /* ── GTK init ── */
    gtk_init(&argc, &argv);

    /* ── CloneMe startup ── */
    load_memory();
    start_face_thread();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += FACE_READY_TIMEOUT_MS / 1000;
    if (sem_timedwait(&face_ready_sem, &ts) != 0)
        tee_err("⚠️  Face thread timeout — continuing\n");

    speak("Welcome back! Your assistant is ready.");

    /* ── Main loop ── */
    while (1) {
        switch (mic_mode) {
            case MODE_MIC:
                start_microphone(VOSK_MODEL);
                break;
            case MODE_TEXT:
                show_text_window();
                break;
            case MODE_QUIT:
            default:
                goto shutdown;
        }
    }

shutdown:
    persist_memory();
    stop_face_thread();
    return 0;
}
