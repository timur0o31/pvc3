#ifndef PASSWORD_STORE_H
#define PASSWORD_STORE_H

#define PASSWORD_LENGTH 8u

typedef enum {
    PASSWORD_STORE_OK,
    PASSWORD_STORE_EMPTY,
    PASSWORD_STORE_CORRUPT,
    PASSWORD_STORE_IO_ERROR,
    PASSWORD_STORE_BAD_ARGUMENT
} PasswordStore_Result;

/* Читает последнюю целую запись. password — ровно 8 байт, без завершающего нуля.
 * EMPTY означает чистую область (все FF или все 00), а не ошибку связи.
 * При ошибке выходной массив не меняется. Вызывать после MX_I2C1_Init(). */
PasswordStore_Result PasswordStore_Load(char password[PASSWORD_LENGTH]);
/* Сохраняет в другую запись, проверяет чтением и только затем завершает операцию.
 * Использует только обычную EEPROM 0x00..0x3F. RTC, SRAM, EUI-48 не затрагивает.
 * Вызывать из main, не из обработчика прерывания. */
PasswordStore_Result PasswordStore_Save(const char password[PASSWORD_LENGTH]);

#endif
