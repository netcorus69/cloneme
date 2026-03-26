#ifndef MIC_H
#define MIC_H

// Start microphone input with a given Vosk model path
void start_microphone(const char *model_path);

// Stop microphone input gracefully
void mic_stop();

#endif
