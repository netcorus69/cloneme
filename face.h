#ifndef FACE_H
#define FACE_H

// Add these two lines to face.h
void hide_face(void);
void show_face(void);

// Start mouth animation (speech begins)
void start_mouth_animation(void);

// Stop mouth animation (speech ends)
void stop_mouth_animation(void);

// Launch the SDL face thread
void start_face_thread(void);

// Stop the SDL face thread
void stop_face_thread(void);

// Animate mouth for estimated duration (optional)
void animate_mouth_for(const char *text);


#endif

