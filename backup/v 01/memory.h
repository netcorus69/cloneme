#ifndef MEMORY_H
#define MEMORY_H

void load_memory();
void persist_memory();
void save_memory(const char *question, const char *answer);
const char* recall_memory(const char *question);
void show_memory();
void forget_memory(const char *question);
void list_memory();
void forget_memory_by_index(int index);

// Accessors
int get_memory_count(void);
const char *get_memory_key(int index);
const char *get_memory_value(int index);

// Wrapper for respond.c
void add_memory(const char *item);

// Helper for GTK/text mode
char* list_memory_as_string();

#endif


