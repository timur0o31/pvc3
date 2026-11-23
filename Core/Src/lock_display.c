#include "lock_display.h"
#include "lock.h"
#include "oled.h"
#include <string.h>

static Lock_Display last;
static bool drawn;

/* Представление отделено от проверки пароля: OLED только читает снимок замка.
 * Незавершённый кадр сначала досылаем; затем берём самое свежее состояние. */
void LockDisplay_Update(void)
{
    if (!OLED_Busy() && OLED_GetStatus() == HAL_OK) {
        Lock_Display view = Lock_GetDisplay();
        if (!drawn || view.state != last.state || view.error != last.error ||
            strcmp(view.input, last.input) != 0) {
            const char *title = "ENTER CODE";
            unsigned scale = 1;
            switch (view.state) {
                case LOCK_STORAGE_FAULT: title = "EEPROM ERROR"; break;
                case LOCK_OPEN: title = "PASSED"; scale = 2; break;
                case LOCK_BLOCKED: title = "BLOCKED"; scale = 2; break;
                case LOCK_EDIT: title = "NEW CODE"; break;
                case LOCK_CONFIRM_EDIT: title = "CONFIRM NEW CODE"; break;
                case LOCK_INPUT:
                    if (view.error) { title = "ERROR"; scale = 2; }
                    break;
            }
            OLED_Clear();
            OLED_Text(0, 0, "CODE LOCK", 1);
            OLED_Text(0, 20, title, scale);
            OLED_Text(0, 44, view.input, 2);
            OLED_Present();
            last = view;
            drawn = true;
        }
    }
    OLED_Update();
}
