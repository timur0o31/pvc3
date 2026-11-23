#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>

typedef enum {
    LOCK_INPUT, LOCK_EDIT, LOCK_CONFIRM_EDIT, LOCK_OPEN, LOCK_BLOCKED,
    LOCK_STORAGE_FAULT
} Lock_State;

typedef enum {
    LOCK_OK, LOCK_GRANTED, LOCK_CONFIRM_REQUIRED, LOCK_CHANGED,
    LOCK_BAD_CODE, LOCK_BAD_CHAR, LOCK_BAD_STATE,
    LOCK_ALREADY_OPEN, LOCK_TEMPORARILY_BLOCKED, LOCK_STORAGE_ERROR
} Lock_Result;

/* Только уже введённые символы, а не сохранённый пароль.
 * Снимок используется экраном; error сохраняется до следующего ввода/сброса. */
typedef struct {
    Lock_State state;
    char input[9];
    bool error;
} Lock_Display;
Lock_Display Lock_GetDisplay(void);

void Lock_Init(void);
void Lock_Update(void);
Lock_State Lock_GetState(void);
bool Lock_FeedbackPending(void);
Lock_Result Lock_Key(char key);
Lock_Result Lock_BeginEdit(void);
Lock_Result Lock_Confirm(void);
Lock_Result Lock_Cancel(void);

#endif
