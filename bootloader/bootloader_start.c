/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_crc.h"
#include "esp_rom_spiflash.h" // The key: direct access to ROM flash functions
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_hooks.h"

static const char *TAG = "boot";

// --- Self-Contained OTA Logic using ROM Functions ---

// Minimal partition entry structure to find otadata
typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    uint8_t label[16];
    uint32_t flags;
} minimal_partition_entry_t;

// Minimal otadata entry structure (older version compatible)
typedef struct {
    uint32_t ota_seq;
    uint8_t ota_state;
    uint8_t reserved[3];
    uint32_t crc;
} minimal_ota_select_entry_t;

// Find the flash offset of the otadata partition by manually scanning the partition table.
static uint32_t find_otadata_offset(void)
{
    const uint32_t partition_table_addr = 0x8000; // ESP_PARTITION_TABLE_OFFSET
    const uint8_t ota_data_subtype = 0x00;       // ESP_PARTITION_SUBTYPE_DATA_OTA
    minimal_partition_entry_t entry;

    for (int i = 0; i < 16; ++i) { // Scan a reasonable number of entries
        uint32_t read_addr = partition_table_addr + i * sizeof(minimal_partition_entry_t);
        // --- FIX: Cast struct pointer to uint32_t* ---
        esp_rom_spiflash_read(read_addr, (uint32_t*)&entry, sizeof(entry));

        if (entry.magic == 0x50AA && entry.type == 0x01 && entry.subtype == ota_data_subtype) {
            return entry.offset;
        }
    }
    return 0; // Not found
}

// The core logic to set the next boot partition to OTA0.
static void set_next_boot_to_recovery(void)
{
    uint32_t otadata_addr = find_otadata_offset();
    if (otadata_addr == 0) {
        ESP_LOGE(TAG, "otadata partition not found. Cannot set next boot.");
        return;
    }

    // Read the two ota_select entries
    minimal_ota_select_entry_t entries[2];
    // --- FIX: Cast struct pointer to uint32_t* ---
    esp_rom_spiflash_read(otadata_addr, (uint32_t*)entries, sizeof(entries));

    // Basic validation and selection of which entry to update
    bool entry0_valid = (entries[0].crc == esp_rom_crc32_le(0, (uint8_t*)&entries[0], sizeof(minimal_ota_select_entry_t) - 4));
    bool entry1_valid = (entries[1].crc == esp_rom_crc32_le(0, (uint8_t*)&entries[1], sizeof(minimal_ota_select_entry_t) - 4));

    uint8_t update_idx = 0;
    if (entry0_valid && entry1_valid) {
        update_idx = (entries[0].ota_seq > entries[1].ota_seq) ? 1 : 0;
    } else if (entry0_valid) {
        update_idx = 1;
    } else {
        update_idx = 0;
    }

    // Prepare the new entry to boot OTA0
    minimal_ota_select_entry_t new_entry;
    uint32_t last_seq = entries[update_idx == 0 ? 1 : 0].ota_seq;
    new_entry.ota_seq = last_seq + 1;
    new_entry.ota_state = 0; // ESP_OTA_IMG_NEW / PENDING_VERIFY
    new_entry.crc = esp_rom_crc32_le(0, (uint8_t*)&new_entry, sizeof(minimal_ota_select_entry_t) - 4);

    // Write to flash using ROM functions
    uint32_t write_addr = otadata_addr + update_idx * sizeof(minimal_ota_select_entry_t);
    esp_rom_spiflash_erase_sector(write_addr / 4096);
    // --- FIX: Cast struct pointer to const uint32_t* ---
    esp_rom_spiflash_write(write_addr, (const uint32_t*)&new_entry, sizeof(new_entry));
    ESP_LOGI(TAG, "Next boot set to OTA0 via ROM functions.");
}


// --- Standard Bootloader Flow ---

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_before_init) {
        bootloader_before_init();
    }
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }
    if (bootloader_after_init) {
        bootloader_after_init();
    }

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    bootloader_utility_load_boot_image(&bs, boot_index);
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }
    return selected_boot_partition(bs);
}

static int selected_boot_partition(const bootloader_state_t *bs)
{
    // First, let the standard logic determine which partition to boot NOW.
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index;
    }

    // Now, inject our logic to change what happens on the NEXT boot.
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        set_next_boot_to_recovery();
    }

    // Return the partition for the current boot.
    return boot_index;
}

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif