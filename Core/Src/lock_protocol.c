#include "lock_protocol.h"
#include "lock.h"
#include "uart_driver.h"
#include <string.h>

#define MAX_COMMAND_LENGTH 24u

static char command[MAX_COMMAND_LENGTH + 1u];
static size_t command_length;
static bool invalid_command;
static bool resync;
static uint32_t rx_errors;
static bool uart_interrupts;
static const char *pending_reply;
static bool wait_for_feedback;
static bool terminal_mode;
static char terminal_reply[64];

static const char *const state_replies[] = {
    [LOCK_INPUT] = "OK STATE INPUT\r\n",
    [LOCK_EDIT] = "OK STATE EDIT\r\n",
    [LOCK_CONFIRM_EDIT] = "OK STATE CONFIRM_EDIT\r\n",
    [LOCK_OPEN] = "OK STATE OPEN\r\n",
    [LOCK_BLOCKED] = "OK STATE BLOCKED\r\n",
    [LOCK_STORAGE_FAULT] = "ERR EEPROM\r\n",
};

static const char *const result_replies[] = {
    [LOCK_OK] = "OK\r\n",
    [LOCK_GRANTED] = "OK OPEN\r\n",
    [LOCK_CONFIRM_REQUIRED] = "OK CONFIRM_REQUIRED\r\n",
    [LOCK_CHANGED] = "OK CHANGED\r\n",
    [LOCK_BAD_CODE] = "ERR BAD_CODE\r\n",
    [LOCK_BAD_CHAR] = "ERR BAD_CHAR\r\n",
    [LOCK_BAD_STATE] = "ERR BAD_STATE\r\n",
    [LOCK_ALREADY_OPEN] = "ERR OPEN\r\n",
    [LOCK_TEMPORARILY_BLOCKED] = "ERR BLOCKED\r\n",
    [LOCK_STORAGE_ERROR] = "ERR EEPROM\r\n",
};

/* Оба режима вызывают одну проверку кода и соблюдают ту же паузу LED. */
static const char *key_reply(char key)
{
    Lock_Result result = Lock_Key(key);
    wait_for_feedback = Lock_GetState() == LOCK_INPUT &&
                        (result == LOCK_OK || result == LOCK_BAD_CODE);
    return result_replies[result];
}

static const char *handle_command(void)
{
    /* Команды можно вводить в любом регистре. Аргумент KEY остаётся как введён. */
    for (char *c = command; *c != '\0' && *c != ' '; c++) {
        if (*c >= 'a' && *c <= 'z') {
            *c = (char)('A' + (*c - 'a'));
        }
    }

    if (strcmp(command, "TERMINAL") == 0) {
        Lock_Result result = Lock_Cancel();
        if (result != LOCK_OK) return result_replies[result];
        terminal_mode = true;
        return "OK TERMINAL\r\n";
    }
    if (strcmp(command, "PING") == 0) {
        return "PONG\r\n";
    }
    if (strcmp(command, "STATUS") == 0) {
        return state_replies[Lock_GetState()];
    }
    if (command_length == 5u && memcmp(command, "KEY ", 4u) == 0) {
        return key_reply(command[4]);
    }
    if (strcmp(command, "EDIT") == 0) {
        return result_replies[Lock_BeginEdit()];
    }
    if (strcmp(command, "CONFIRM") == 0) {
        return result_replies[Lock_Confirm()];
    }
    if (strcmp(command, "CANCEL") == 0) {
        return result_replies[Lock_Cancel()];
    }
    return "ERR COMMAND\r\n";
}

static bool is_line_end(int byte)
{
    return byte == '\r' || byte == '\n';
}

/* В этом режиме буквенно-цифровой байт сразу является клавишей замка.
 * Enter игнорируется. Backspace сбрасывает ВЕСЬ текущий ввод: отдельный символ
 * уже проверен замком и не может быть просто удалён из командной строки.
 * Esc возвращает протокол команд. Изменение пароля выполняется там через EDIT.
 * Эхо и результат храним до отправки: pending_reply защищает буфер от перезаписи. */
static const char *consume_terminal_byte(int byte)
{
    if (byte == 0x1b) {
        terminal_mode = false;
        command_length = 0;
        invalid_command = false;
        Lock_Cancel();
        return "OK COMMANDS\r\n";
    }
    if (is_line_end(byte)) return NULL;
    if (byte == '\b' || byte == 0x7f) return result_replies[Lock_Cancel()];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9'))) return "ERR BAD_CHAR\r\n";
    const char *reply = key_reply((char)byte);
    terminal_reply[0] = (char)byte;
    memcpy(terminal_reply + 1, " -> ", 4);
    strcpy(terminal_reply + 5, reply);
    return terminal_reply;
}

static const char *consume_byte(int byte)
{
    /* Enter в последовательном терминале может отправлять CR или LF.
       Пустой кадр игнорируется, поэтому CRLF даёт один ответ. */
    if (is_line_end(byte)) {
        const char *reply = NULL;
        if (invalid_command) {
            reply = "ERR FRAME\r\n";
        } else if (command_length != 0u) {
            command[command_length] = '\0';
            reply = handle_command();
        }
        command_length = 0;
        invalid_command = false;
        return reply;
    }

    if (invalid_command) {
        return NULL;
    }
    if (byte == '\b' || byte == 0x7f) {
        if (command_length != 0u) {
            command_length--;
        }
    } else if (byte < 0x20 || byte > 0x7e || command_length == MAX_COMMAND_LENGTH) {
        invalid_command = true;
    } else {
        command[command_length++] = (char)byte;
    }
    return NULL;
}

static bool recover_rx_error(void)
{
    if (UART_RxErrors() == rx_errors) {
        return false;
    }
    /* После потери байта обрабатываем команды только с новой строки. */
    rx_errors = UART_DiscardRx();
    command_length = 0;
    invalid_command = false;
    resync = true;
    terminal_mode = false; /* После потери байтов снова требуем командный режим. */
    Lock_Cancel();
    pending_reply = "ERR UART\r\n";
    return true;
}

void LockProtocol_Init(void)
{
    command_length = 0;
    invalid_command = false;
    resync = false;
    terminal_mode = false;
    rx_errors = UART_RxErrors();
    uart_interrupts = UART_InterruptsEnabled();
    pending_reply = NULL;
    wait_for_feedback = false;
}

void LockProtocol_Update(void)
{
    if (pending_reply != NULL) {
        if (wait_for_feedback && Lock_FeedbackPending()) {
            return;
        }
        if (UART_Print(pending_reply)) {
            pending_reply = NULL;
            wait_for_feedback = false;
        }
        return;
    }
    if (recover_rx_error()) {
        return;
    }

    bool interrupts = UART_InterruptsEnabled();
    if (interrupts != uart_interrupts) {
        uart_interrupts = interrupts;
        pending_reply = interrupts ? "EVT UART IRQ\r\n" : "EVT UART POLL\r\n";
        return;
    }

    int byte = UART_ReadByte();
    if (byte < 0 || recover_rx_error()) {
        return;
    }
    if (resync) {
        resync = !is_line_end(byte);
        return;
    }
    pending_reply = terminal_mode ? consume_terminal_byte(byte) : consume_byte(byte);
}
