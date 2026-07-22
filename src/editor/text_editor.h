#pragma once

#include "editor_config.h"

bool editorInit();  // Allocates the text/line buffers; false = out of memory
void editorFree();  // Returns the buffers to the heap (safe to call when unallocated)
void editorClear();
void editorLoadBuffer(size_t length);  // After filling buffer externally, set length + reset cursor

// Buffer access
char* editorGetBuffer();
size_t editorGetLength();
int editorGetCursorPosition();

// Editing operations
void editorInsertChar(char c);
void editorDeleteChar();     // Backspace
void editorDeleteForward();  // Delete key

// Cursor movement
void editorMoveCursorLeft();
void editorMoveCursorRight();
void editorMoveCursorUp();
void editorMoveCursorDown();
void editorMoveCursorHome();
void editorMoveCursorEnd();

// Line/viewport management
void editorSetCharsPerLine(int cpl);
void editorSetVisibleLines(int n);  // Tell editor how many lines are visible on screen
int editorGetStoredVisibleLines();  // Get the last set visible lines count
void editorRecalculateLines();
int editorGetVisibleLines(int lineHeight, int textAreaHeight);
int editorGetViewportStart();
int editorGetCursorLine();
int editorGetCursorCol();
int editorGetLineCount();
int editorGetLinePosition(int lineIndex);

// File metadata
void editorSetCurrentFile(const char* filename);
void editorSetCurrentTitle(const char* title);
const char* editorGetCurrentFile();
const char* editorGetCurrentTitle();
bool editorHasUnsavedChanges();
void editorSetUnsavedChanges(bool v);
int editorGetWordCount();
