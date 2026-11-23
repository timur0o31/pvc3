#include "lock.h"
#include "password_store.h"
#include "main.h"
#include "uart_driver.h"
#include <string.h>

#define CODE_LENGTH PASSWORD_LENGTH
#define INPUT_TIMEOUT_MS 5000u
#define EDIT_TIMEOUT_MS 30000u
#define OPEN_LOCK_MS 3000u
#define BLOCK_LOCK_MS 5000u
#define BLOCK_BLINK_MS 250u
#define LED_FLASH_MS 100u
#define LED_GAP_MS 60u
#define DEBOUNCE_MS 25u

#define GREEN_LED GPIO_PIN_13
#define YELLOW_LED GPIO_PIN_14
#define RED_LED GPIO_PIN_15

static char password[CODE_LENGTH];
static char candidate[CODE_LENGTH];
static char display_input[CODE_LENGTH + 1u];
static bool display_error;
static Lock_State state;
static uint8_t position;
static uint8_t wrong;
static uint32_t state_at;
static uint32_t entry_at;
static uint32_t feedback_at;
static bool feedback_pending;

static bool button_raw;
static bool button_stable;
static bool mode_change_pending;
static uint32_t button_at;

static void leds(uint16_t pins)
{
    HAL_GPIO_WritePin(GPIOD, GREEN_LED | YELLOW_LED | RED_LED, GPIO_PIN_RESET);
    if (pins != 0u) {
        HAL_GPIO_WritePin(GPIOD, pins, GPIO_PIN_SET);
    }
}

static char to_lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool valid_key(char c)
{
    c = to_lower_ascii(c);
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
}

static void reset_input(void)
{
    state = LOCK_INPUT;
    position = 0;
    memset(display_input, 0, sizeof display_input);
    display_error = false;
    feedback_pending = false;
    leds(0);
}

static void flash(uint16_t pin, uint32_t now)
{
    feedback_pending = true;
    feedback_at = now;
    leds(pin);
}

static Lock_Result check_key(char key, uint32_t now)
{
    /* Сохраняем введённый символ до проверки: при ошибке он тоже виден. */
    if (position == 0u) memset(display_input, 0, sizeof display_input);
    display_input[position] = key;
    display_input[position + 1u] = '\0';
    display_error = false;
    if (to_lower_ascii(key) != to_lower_ascii(password[position])) {
        position = 0;
        wrong++;
        display_error = true;
        if (wrong >= 3u) {
            state = LOCK_BLOCKED;
            state_at = now;
            feedback_pending = false;
            leds(RED_LED);
            return LOCK_TEMPORARILY_BLOCKED;
        }
        flash(RED_LED, now);
        return LOCK_BAD_CODE;
    }

    if (position == 0u) {
        entry_at = now;
    }
    position++;
    if (position == CODE_LENGTH) {
        position = 0;
        wrong = 0;
        state = LOCK_OPEN;
        state_at = now;
        feedback_pending = false;
        leds(GREEN_LED);
        return LOCK_GRANTED;
    }
    flash(YELLOW_LED, now);
    return LOCK_OK;
}

static Lock_Result unavailable(void)
{
    switch (state) {
        case LOCK_STORAGE_FAULT:
            return LOCK_STORAGE_ERROR;
        case LOCK_OPEN:
            return LOCK_ALREADY_OPEN;
        case LOCK_BLOCKED:
            return LOCK_TEMPORARILY_BLOCKED;
        default:
            return LOCK_BAD_STATE;
    }
}

Lock_Result Lock_Key(char key)
{
    uint32_t now = HAL_GetTick();
    if (state == LOCK_INPUT) {
        return check_key(key, now);
    }
    if (state != LOCK_EDIT) {
        return unavailable();
    }
    if (!valid_key(key)) {
        return LOCK_BAD_CHAR;
    }
    candidate[position++] = key;
    memcpy(display_input, candidate, position);
    display_input[position] = '\0';
    state_at = now;
    if (position == CODE_LENGTH) {
        state = LOCK_CONFIRM_EDIT;
        return LOCK_CONFIRM_REQUIRED;
    }
    return LOCK_OK;
}

Lock_Result Lock_BeginEdit(void)
{
    if (state != LOCK_INPUT) {
        return unavailable();
    }
    reset_input();
    state = LOCK_EDIT;
    state_at = HAL_GetTick();
    return LOCK_OK;
}

Lock_Result Lock_Confirm(void)
{
    if (state != LOCK_CONFIRM_EDIT) {
        return unavailable();
    }
    /* RAM меняем только после подтверждённой записи EEPROM.
     * Если связь потеряна в конце записи, её результат может быть неизвестен.
     * До перезапуска и повторного чтения не разрешаем проверку старого пароля. */
    if (PasswordStore_Save(candidate) != PASSWORD_STORE_OK) {
        state = LOCK_STORAGE_FAULT;
        feedback_pending = false;
        leds(RED_LED);
        return LOCK_STORAGE_ERROR;
    }
    memcpy(password, candidate, sizeof password);
    wrong = 0;
    reset_input();
    return LOCK_CHANGED;
}

Lock_Result Lock_Cancel(void)
{
    if (state == LOCK_OPEN || state == LOCK_BLOCKED || state == LOCK_STORAGE_FAULT) {
        return unavailable();
    }
    reset_input();
    return LOCK_OK;
}

Lock_Display Lock_GetDisplay(void)
{
    Lock_Display result = { .state = state, .error = display_error };
    memcpy(result.input, display_input, sizeof result.input);
    return result;
}

Lock_State Lock_GetState(void)
{
    return state;
}

bool Lock_FeedbackPending(void)
{
    return feedback_pending;
}

static bool button_pressed(void)
{
    return HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_15) == GPIO_PIN_RESET;
}

void Lock_Init(void)
{
    /* Только чистая EEPROM получает начальный пароль. Сбой чтения или
     * повреждение обеих записей не подменяем паролем по умолчанию. */
    memset(password, 0, sizeof password);
    PasswordStore_Result loaded = PasswordStore_Load(password);
    if (loaded == PASSWORD_STORE_EMPTY) {
        loaded = PasswordStore_Save("Ab12Cd34");
        if (loaded == PASSWORD_STORE_OK) memcpy(password, "Ab12Cd34", sizeof password);
    }
    wrong = 0;
    reset_input();
    button_raw = button_pressed();
    button_stable = button_raw;
    button_at = HAL_GetTick();
    mode_change_pending = false;
    if (loaded != PASSWORD_STORE_OK) {
        state = LOCK_STORAGE_FAULT;
        leds(RED_LED);
    }
}

static void update_button(uint32_t now)
{
    bool pressed = button_pressed();
    if (pressed != button_raw) {
        button_raw = pressed;
        button_at = now;
    }
    if (button_stable != button_raw && now - button_at >= DEBOUNCE_MS) {
        button_stable = button_raw;
        if (button_stable) {
            mode_change_pending = true;
        }
    }
    if (mode_change_pending && UART_SetInterrupts(!UART_InterruptsEnabled())) {
        mode_change_pending = false;
    }
}

static void update_state(uint32_t now)
{
    uint32_t elapsed = now - state_at;
    switch (state) {
        case LOCK_STORAGE_FAULT:
            break;
        case LOCK_OPEN:
            if (elapsed >= OPEN_LOCK_MS) {
                reset_input();
            }
            break;
        case LOCK_BLOCKED:
            if (elapsed >= BLOCK_LOCK_MS) {
                wrong = 0;
                reset_input();
            } else {
                leds((elapsed / BLOCK_BLINK_MS) % 2u == 0u ? RED_LED : 0u);
            }
            break;
        case LOCK_INPUT:
            if (position != 0u && now - entry_at >= INPUT_TIMEOUT_MS) {
                position = 0;
                memset(display_input, 0, sizeof display_input);
                display_error = true;
                flash(RED_LED, now);
            }
            break;
        case LOCK_EDIT:
        case LOCK_CONFIRM_EDIT:
            if (elapsed >= EDIT_TIMEOUT_MS) {
                reset_input();
            }
            break;
    }
}

void Lock_Update(void)
{
    uint32_t now = HAL_GetTick();
    update_button(now);
    if (feedback_pending && now - feedback_at >= LED_FLASH_MS) {
        leds(0);
        feedback_pending = now - feedback_at < LED_FLASH_MS + LED_GAP_MS;
    }
    update_state(now);
}
