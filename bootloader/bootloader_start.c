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

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

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

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // If this boot is a wake up from the deep sleep then go to the short way,
    // try to load the application which worked before deep sleep.
    // It skips a lot of checks due to it was done before (while first boot).
    bootloader_utility_load_boot_image_from_deep_sleep();
    // If it is not successful try to load an application as usual.
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 2.1 Load the TEE image
#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    ESP_LOGI(TAG, "--- Checking button on GPIO %d ---", BOOT_BUTTON_GPIO);
    bool button = button_pressed();
    ESP_LOGI(TAG, "Button state is: %d (%s)", button, button ? "PRESSED" : "NOT PRESSED");

    // --- Select OTA partition based on button ---
    if (button) {
        // Button pressed → OTA_0
        boot_index = 0;   // OTA_0 partition index
    } else {
        // Button not pressed → OTA_1
        boot_index = 1;   // OTA_1 partition index
    }


    // 3. Load the app image for booting
    //while (true) {}
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return selected_boot_partition(bs);
}

/*
 * Selects a boot partition.
 * The conditions for switching to another firmware are checked.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index; // Unrecoverable failure (not due to corrupt ota data or bad partition contents)
    }
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        // Factory firmware.
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
        bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
        reset_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, reset_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a condition of the factory reset");
            bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
            ota_data_erase = true;
#endif
            const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
            ESP_LOGI(TAG, "Data partitions to erase: %s", list_erase);
            if (bootloader_common_erase_part_type_data(list_erase, ota_data_erase) == false) {
                ESP_LOGE(TAG, "Not all partitions were erased");
            }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
            bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
            return bootloader_utility_get_selected_boot_partition(bs);
        }
#endif // CONFIG_BOOTLOADER_FACTORY_RESET
        // TEST firmware.
#ifdef CONFIG_BOOTLOADER_APP_TEST
        bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
        app_test_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_APP_TEST, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, app_test_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a boot condition of the test firmware");
            if (bs->test.offset != 0) {
                boot_index = TEST_APP_INDEX;
                return boot_index;
            } else {
                ESP_LOGE(TAG, "Test firmware is not found in partition table");
                return INVALID_INDEX;
            }
        }
#endif // CONFIG_BOOTLOADER_APP_TEST
        // Customer implementation.
        // if (gpio_pin_1 == true && ...){
        //     boot_index = required_boot_partition;
        // } ...
    }
    return boot_index;
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
