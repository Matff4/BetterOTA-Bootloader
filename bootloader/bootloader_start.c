/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_hooks.h"
#include "soc/gpio_struct.h"

static const char *TAG = "BetterOTA";

static int choose_ota_partition(const bootloader_state_t *bs);

// --- Button Configuration ---
static const uint8_t BOOT_BUTTON_GPIO = 13;
static const uintptr_t IO_MUX_MTCK_REG_ADDR = 0x3FF49038U;
static volatile uint32_t * const IO_MUX_MTCK_REG = (volatile uint32_t *)IO_MUX_MTCK_REG_ADDR;


/**
 * @brief Initializes the boot button on GPIO 13.
 */
static void button_init(void)
{
    uint32_t r = *IO_MUX_MTCK_REG;
    r &= ~(0x7U << 12);           // clear MCU_SEL
    r |= (2U << 12);              // set MCU_SEL = 2 (GPIO)
    r &= ~(1U << 7);              // clear pulldown
    r |= (1U << 8);               // set pullup
    r |= (1U << 9);               // enable input (FUN_IE)
    *IO_MUX_MTCK_REG = r;
}

/**
 * @brief Reads button state using the reliable "drive test" method.
 *
 * This function forces the pin high momentarily, which correctly initializes
 * it, allowing for a reliable read. It then safely returns the pin to
 * a high-impedance input state with the pull-up re-enabled.
 *
 * @return int 0 if the button is pressed (LOW), 1 if not pressed (HIGH).
 */
static bool button_pressed(void)
{
    button_init();          // Initial setup

    GPIO.enable_w1ts = (1u << BOOT_BUTTON_GPIO);    // Enable output for the pin
    GPIO.out_w1ts = (1u << BOOT_BUTTON_GPIO);       // Set the output level to high

    const bool pressed = !(GPIO.in & (1u << BOOT_BUTTON_GPIO));  // Read the pin state while it's being driven

    GPIO.enable_w1tc = (1u << BOOT_BUTTON_GPIO);    // Immediately disable output, returning pin to a safe high-impedance state
    GPIO.out_w1tc = (1u << BOOT_BUTTON_GPIO);       // Clear the output register just in case

    button_init();          // Re-initialize the pin to ensure the pull-up is active again

    return pressed;
}


/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // (0. Call the before-init hook, if available)
    if (bootloader_before_init) {
        bootloader_before_init();
    }

    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    // (1.1 Call the after-init hook, if available)
    if (bootloader_after_init) {
        bootloader_after_init();
    }

    ESP_LOGE(TAG, "BetterOTA Bootloader v0.1 loaded successfully");

    // --- Select the OTA partition based on button ---
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "Failed to load partition table!");
        bootloader_reset();
    }

    int boot_index = choose_ota_partition(&bs);

    // Boot the selected partition
    bootloader_utility_load_boot_image(&bs, boot_index);
    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

/**
 * @brief Chooses the OTA partition index based on the boot button.
 *
 * @param bs Pointer to the bootloader_state_t (for partition table info if needed)
 * @return int Index of the partition to boot (0 = OTA_0, 1 = OTA_1)
 */
static int choose_ota_partition(const bootloader_state_t *bs)
{
    // Read button
    bool button = button_pressed();

    ESP_LOGI(TAG, "Button state is: %d (%s)", button, button ? "PRESSED" : "NOT PRESSED");

    // OTA selection: pressed → OTA_0, not pressed → OTA_1
    int boot_index = button ? 0 : 1;

    ESP_LOGI(TAG, "Selected boot partition index: %d", boot_index);

    return boot_index;
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
