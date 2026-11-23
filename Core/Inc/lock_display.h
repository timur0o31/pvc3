#ifndef LOCK_DISPLAY_H
#define LOCK_DISPLAY_H
/* Вызывать из main после Lock_Update/LockProtocol_Update.
 * Формирует изображение при изменении замка и передаёт его частями. */
void LockDisplay_Update(void);
#endif
