/* MCP79411, DS20002266H, разделы 5.1 и 6.2:
 * https://ww1.microchip.com/downloads/en/DeviceDoc/20002266H.pdf
 * EEPROM: адрес I2C 0x57 (HAL требует 0xAE), 128 байт, страницы по 8 байт.
 * HAL_Mem_Read выполняет установку адреса и repeated START перед чтением.
 */
#include "password_store.h"
#include "i2c.h"
#include "uart_driver.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define EEPROM_ADDRESS (0x57u << 1)
#define PAGE_SIZE 8u
#define SLOT_SIZE 32u
#define BODY_SIZE 24u
#define COMMIT_OFFSET 24u
#define TRANSFER_SIZE 4u
#define IO_TIMEOUT_MS 10u
#define WRITE_TIMEOUT_MS 15u

/* Две записи занимают 0x00..0x1F и 0x20..0x3F.
 * 0..3: формат LCK1; 4..7: номер записи LE; 8..15: пароль;
 * 16..19: CRC32 первых 16 байт; 20..23: нули;
 * 24..31: признак завершённой записи на отдельной странице.
 * CRC обнаруживает повреждения, но не является шифрованием пароля. */
static const uint8_t commit_marker[8] = {0xA5, 0x5A, 'O', 'K', 1, 0xFE, 0x55, 0xAA};

typedef struct {
    uint8_t bytes[2][SLOT_SIZE];
    int current;
} Records;

/* Пока EEPROM программирует ячейки (до 5 мс по паспорту), она отвечает NACK.
 * Ограничиваем время и количество попыток. UART обслуживается между опросами. */
static bool wait_ready(void)
{
    uint32_t start = HAL_GetTick();
    for (unsigned attempt = 0; attempt < 64u; attempt++) {
        UART_Service();
        if (HAL_I2C_IsDeviceReady(&hi2c1, EEPROM_ADDRESS, 1, 2) == HAL_OK)
            return true;
        if ((uint32_t)(HAL_GetTick() - start) >= WRITE_TIMEOUT_MS) break;
    }
    return false;
}

/* Малые транзакции оставляют main возможность обслужить UART в режиме опроса. */
static bool read_bytes(uint16_t address, uint8_t *data, unsigned length)
{
    if (!wait_ready()) return false;
    while (length != 0u) {
        unsigned count = length < TRANSFER_SIZE ? length : TRANSFER_SIZE;
        UART_Service();
        if (HAL_I2C_Mem_Read(&hi2c1, EEPROM_ADDRESS, address,
                            I2C_MEMADD_SIZE_8BIT, data, count, IO_TIMEOUT_MS) != HAL_OK)
            return false;
        address += count;
        data += count;
        length -= count;
    }
    UART_Service();
    return true;
}

/* Никогда не пересекаем границу страницы: иначе адрес внутри EEPROM завернётся
 * к началу той же страницы. После каждого STOP ждём завершения записи по ACK. */
static bool write_bytes(uint16_t address, const uint8_t *data, unsigned length)
{
    while (length != 0u) {
        unsigned count = PAGE_SIZE - address % PAGE_SIZE;
        if (count > length) count = length;
        if (count > TRANSFER_SIZE) count = TRANSFER_SIZE;
        UART_Service();
        if (HAL_I2C_Mem_Write(&hi2c1, EEPROM_ADDRESS, address,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)data,
                             count, IO_TIMEOUT_MS) != HAL_OK || !wait_ready())
            return false;
        address += count;
        data += count;
        length -= count;
    }
    return true;
}

/* Формат байтов задаём явно, независимо от выравнивания структур компилятором. */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4u; i++) p[i] = (uint8_t)(value >> (i * 8u));
}

/* CRC-32/ISO-HDLC: полином 0xEDB88320, начальное и финальное XOR = FFFFFFFF. */
static uint32_t crc32(const uint8_t *data, unsigned length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool valid_password(const char *password)
{
    if (password == NULL) return false;
    for (unsigned i = 0; i < PASSWORD_LENGTH; i++) {
        char c = password[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

static bool valid_record(const uint8_t *record)
{
    return memcmp(record, "LCK1", 4) == 0 &&
           memcmp(record + COMMIT_OFFSET, commit_marker, sizeof commit_marker) == 0 &&
           get_u32(record + 16) == crc32(record, 16) &&
           get_u32(record + 20) == 0u && valid_password((const char *)record + 8);
}

/* Выбирает новейшую целую запись, включая переход счётчика FFFFFFFF -> 0.
 * При ошибке чтения не делаем вывод, что память пуста, и ничего не записываем. */
static PasswordStore_Result read_records(Records *records)
{
    records->current = -1;
    for (unsigned i = 0; i < 2u; i++) {
        if (!read_bytes(i * SLOT_SIZE, records->bytes[i], SLOT_SIZE))
            return PASSWORD_STORE_IO_ERROR;
        if (valid_record(records->bytes[i])) {
            if (records->current < 0) records->current = (int)i;
            else {
                uint32_t delta = get_u32(records->bytes[i] + 4) -
                                 get_u32(records->bytes[records->current] + 4);
                if (delta != 0u && delta < 0x80000000u) records->current = (int)i;
            }
        }
    }
    if (records->current >= 0) return PASSWORD_STORE_OK;
    bool erased = true, zeroed = true;
    for (unsigned i = 0; i < 2u; i++) {
        for (unsigned j = 0; j < SLOT_SIZE; j++) {
            erased = erased && records->bytes[i][j] == 0xFFu;
            zeroed = zeroed && records->bytes[i][j] == 0u;
        }
    }
    return (erased || zeroed) ? PASSWORD_STORE_EMPTY : PASSWORD_STORE_CORRUPT;
}

PasswordStore_Result PasswordStore_Load(char password[PASSWORD_LENGTH])
{
    if (password == NULL) return PASSWORD_STORE_BAD_ARGUMENT;
    Records records;
    PasswordStore_Result result = read_records(&records);
    if (result == PASSWORD_STORE_OK)
        memcpy(password, records.bytes[records.current] + 8, PASSWORD_LENGTH);
    return result;
}

PasswordStore_Result PasswordStore_Save(const char password[PASSWORD_LENGTH])
{
    if (!valid_password(password)) return PASSWORD_STORE_BAD_ARGUMENT;
    Records records;
    PasswordStore_Result result = read_records(&records);
    if (result != PASSWORD_STORE_OK && result != PASSWORD_STORE_EMPTY) return result;
    if (records.current >= 0 &&
        memcmp(records.bytes[records.current] + 8, password, PASSWORD_LENGTH) == 0)
        return PASSWORD_STORE_OK; /* Не расходуем ресурс EEPROM без изменений. */

    unsigned next = records.current < 0 ? 0u : (unsigned)(1 - records.current);
    uint16_t base = next * SLOT_SIZE;
    uint8_t record[SLOT_SIZE] = {0};
    memcpy(record, "LCK1", 4);
    uint32_t generation = records.current < 0 ? 0u :
                          get_u32(records.bytes[records.current] + 4) + 1u;
    put_u32(record + 4, generation);
    memcpy(record + 8, password, PASSWORD_LENGTH);
    put_u32(record + 16, crc32(record, 16));
    uint8_t verify[SLOT_SIZE];
    uint8_t invalid[8] = {0};

    /* Сначала гасим и проверяем маркер целевой записи. Действующую не трогаем.
     * Защищённая от записи память может ACK-нуть команду без изменения данных,
     * поэтому проверки чтением обязательны. STATUS/BP автоматически не меняем. */
    if (!write_bytes(base + COMMIT_OFFSET, invalid, sizeof invalid) ||
        !read_bytes(base + COMMIT_OFFSET, verify, sizeof invalid) ||
        memcmp(verify, invalid, sizeof invalid) != 0)
        return PASSWORD_STORE_IO_ERROR;
    if (!write_bytes(base, record, BODY_SIZE) ||
        !read_bytes(base, verify, BODY_SIZE) || memcmp(record, verify, BODY_SIZE) != 0)
        return PASSWORD_STORE_IO_ERROR;
    memcpy(record + COMMIT_OFFSET, commit_marker, sizeof commit_marker);
    if (!write_bytes(base + COMMIT_OFFSET, commit_marker, sizeof commit_marker) ||
        !read_bytes(base, verify, SLOT_SIZE) || memcmp(record, verify, SLOT_SIZE) != 0)
        return PASSWORD_STORE_IO_ERROR;
    return PASSWORD_STORE_OK;
}
