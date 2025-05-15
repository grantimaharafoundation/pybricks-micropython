// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024 The Pybricks Authors

// Provides Human Machine Interface (HMI) between hub and user.

// TODO: implement additional buttons and menu system (via matrix display) for SPIKE Prime
// TODO: implement additional buttons and menu system (via screen) for NXT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <contiki.h>

#include <pbdrv/core.h>
#include <pbdrv/reset.h>
#include <pbdrv/led.h>
#include <pbio/button.h>
#include <pbio/color.h>
#include <pbio/event.h>
#include <pbio/light.h>
#include <pbsys/config.h>
#include <pbsys/main.h>
#include <pbsys/status.h>
#include <pbsys/storage_settings.h>

#include "light_matrix.h"
#include "light.h"

// Durations for button holds
#define BLUETOOTH_TOGGLE_HOLD_DURATION_MS 1500
#define SHUTDOWN_HOLD_DURATION_MS 3000

static struct pt update_program_run_button_wait_state_pt;
static uint32_t center_button_press_start_time = 0;
static bool bluetooth_toggle_action_taken_this_press = false;

// The selected slot is not persistent across reboot, so that the first slot
// is always active on boot. This allows consistently starting programs without
// visibility of the display.
static uint8_t selected_slot = 0;

/**
 * Protothread to monitor the button state to trigger starting the user program.
 * @param [in]  button_pressed      The current button state.
 */
static PT_THREAD(update_program_run_button_wait_state(bool button_pressed)) {
    struct pt *pt = &update_program_run_button_wait_state_pt;

    // This protothread handles short click actions:
    // - If a program is running: shutdown.
    // - If idle: start program.

    PT_BEGIN(pt);

    for (;;) {
        // Wait for a full press and release cycle (short click)
        PT_WAIT_UNTIL(pt, !button_pressed); // Ensure button is released initially or after a hold
        PT_WAIT_UNTIL(pt, button_pressed);  // Wait for press
        PT_WAIT_UNTIL(pt, !button_pressed); // Wait for release

        // Action depends on whether a program is running
        if (pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING)) {
            // Program is running: short click means shutdown
            pbsys_status_set(PBIO_PYBRICKS_STATUS_SHUTDOWN_REQUEST);
        } else {
            // Program not running (idle): short click means start program
            // Ensure this doesn't happen if a long press shutdown was already initiated for this press event
            if (center_button_press_start_time == 0) { // Indicates button was released from a short click, not a long hold that triggered shutdown
                pbsys_main_program_request_start(selected_slot, PBSYS_MAIN_PROGRAM_START_REQUEST_TYPE_HUB_UI);
            }
        }
    }

    PT_END(pt);
}

#if PBSYS_CONFIG_BLUETOOTH_TOGGLE

static struct pt update_bluetooth_button_wait_state_pt;
static bool waiting_for_bt_toggle_release = false;


/**
 * Protothread to monitor the button state to toggle Bluetooth on long press
 * *while a program is running*.
 * @param [in]  button_pressed      The current button state from pbsys_hmi_poll.
 */
static PT_THREAD(update_bluetooth_button_wait_state(bool hmi_poll_button_is_pressed)) {
    struct pt *pt = &update_bluetooth_button_wait_state_pt;
    static uint32_t press_time_marker;

    PT_BEGIN(pt);

    for (;;) {
        // Only active if a user program is running.
        // And not already actioned this press for BT toggle and waiting for release.
        PT_WAIT_UNTIL(pt, hmi_poll_button_is_pressed &&
                      pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING) &&
                      !waiting_for_bt_toggle_release);

        // Record time when qualified press starts
        press_time_marker = pbdrv_clock_get_ms();

        // Wait while button is pressed, program is running, and duration is less than toggle time
        // And ensure we haven't already taken action for this specific press sequence
        PT_WAIT_WHILE(pt, hmi_poll_button_is_pressed &&
                      pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING) &&
                      (pbdrv_clock_get_ms() - press_time_marker < BLUETOOTH_TOGGLE_HOLD_DURATION_MS) &&
                      !bluetooth_toggle_action_taken_this_press);

        // If button is still pressed, program running, and duration met, and no action yet taken
        if (hmi_poll_button_is_pressed &&
            pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING) &&
            (pbdrv_clock_get_ms() - press_time_marker >= BLUETOOTH_TOGGLE_HOLD_DURATION_MS) &&
            !bluetooth_toggle_action_taken_this_press) {
            pbsys_storage_settings_bluetooth_enabled_request_toggle();
            bluetooth_toggle_action_taken_this_press = true; // Mark action taken for this press cycle
            waiting_for_bt_toggle_release = true; // Wait for release before trying again
        }

        // If button released or program stopped, or action taken, wait for button to be not pressed to reset
        PT_WAIT_UNTIL(pt, !hmi_poll_button_is_pressed || !pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING));
        if (!hmi_poll_button_is_pressed) {
            waiting_for_bt_toggle_release = false; // Reset for next press cycle
            // bluetooth_toggle_action_taken_this_press is reset in pbsys_hmi_poll on new press
        }
    }
    PT_END(pt);
}

#endif // PBSYS_CONFIG_BLUETOOTH_TOGGLE

#if PBSYS_CONFIG_HMI_NUM_SLOTS

static struct pt update_left_right_button_wait_state_pt;

/**
 * Gets the currently selected program slot.
 *
 * @return The currently selected program slot (zero-indexed).
 */
uint8_t pbsys_hmi_get_selected_program_slot(void) {
    return selected_slot;
}

/**
 * Protothread to monitor the left and right button state to select a slot.
 *
 * @param [in]  left_button_pressed      The current left button state.
 * @param [in]  right_button_pressed     The current right button state.
 */
static PT_THREAD(update_left_right_button_wait_state(bool left_button_pressed, bool right_button_pressed)) {
    struct pt *pt = &update_left_right_button_wait_state_pt;

    // This should not be active while a program is running.
    if (pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING)) {
        PT_EXIT(pt);
    }

    static uint8_t previous_slot;
    static uint32_t first_press_time;

    PT_BEGIN(pt);

    for (;;) {
        // Buttons may still be pressed during user program
        PT_WAIT_UNTIL(pt, !left_button_pressed && !right_button_pressed);

        // Wait for either button.
        PT_WAIT_UNTIL(pt, left_button_pressed || right_button_pressed);

        first_press_time = pbdrv_clock_get_ms();

        // On right, increment slot when possible.
        if (right_button_pressed && selected_slot < 4) {
            selected_slot++;
            pbsys_hub_light_matrix_update_program_slot();
        }
        // On left, decrement slot when possible.
        if (left_button_pressed && selected_slot > 0) {
            selected_slot--;
            pbsys_hub_light_matrix_update_program_slot();
        }

        // Next state could be either both pressed or both released.
        PT_WAIT_UNTIL(pt, left_button_pressed == right_button_pressed);

        // If both were pressed soon after another, user wanted to start port view,
        // not switch programs, so revert slot change.
        if (left_button_pressed && pbdrv_clock_get_ms() - first_press_time < 100) {
            selected_slot = previous_slot;
            pbsys_hub_light_matrix_update_program_slot();
            pbsys_main_program_request_start(PBIO_PYBRICKS_USER_PROGRAM_ID_PORT_VIEW, PBSYS_MAIN_PROGRAM_START_REQUEST_TYPE_HUB_UI);
        } else {
            // Successful switch. And UI was already updated.
            previous_slot = selected_slot;
        }
    }

    PT_END(pt);
}

#endif // PBSYS_CONFIG_HMI_NUM_SLOTS

void pbsys_hmi_init(void) {
    pbsys_status_light_init();
    pbsys_hub_light_matrix_init();
    PT_INIT(&update_program_run_button_wait_state_pt);

    #if PBSYS_CONFIG_BLUETOOTH_TOGGLE
    PT_INIT(&update_bluetooth_button_wait_state_pt);
    #endif // PBSYS_CONFIG_BLUETOOTH_TOGGLE
}

void pbsys_hmi_handle_event(process_event_t event, process_data_t data) {
    pbsys_status_light_handle_event(event, data);
    pbsys_hub_light_matrix_handle_event(event, data);

    #if PBSYS_CONFIG_BATTERY_CHARGER
    // On the Technic Large hub, USB can keep the power on even though we are
    // "shutdown", so if the button is pressed again, we reset to turn back on
    if (
        pbsys_status_test(PBIO_PYBRICKS_STATUS_SHUTDOWN)
        && event == PBIO_EVENT_STATUS_SET
        && (pbio_pybricks_status_t)data == PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED
        ) {
        pbdrv_reset(PBDRV_RESET_ACTION_RESET);
    }
    #endif // PBSYS_CONFIG_BATTERY_CHARGER
}

/**
 * Polls the HMI.
 *
 * This is called periodically to update the current HMI state.
 */
void pbsys_hmi_poll(void) {
    pbio_button_flags_t btn_flags;
    bool center_button_currently_pressed = false;

    if (pbio_button_is_pressed(&btn_flags) == PBIO_SUCCESS) {
        if (btn_flags & PBIO_BUTTON_CENTER) {
            center_button_currently_pressed = true;
        }
    }

    if (center_button_currently_pressed) {
        if (center_button_press_start_time == 0) {
            // First detection of this press
            center_button_press_start_time = pbdrv_clock_get_ms();
            bluetooth_toggle_action_taken_this_press = false; // Reset for this new press
            pbsys_status_set(PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED); // For visual feedback if any
        }

        // Call protothreads that react to pressed state
        // update_program_run_button_wait_state is for short clicks, primarily acts on release.
        // update_bluetooth_button_wait_state is for long press while program running.
        #if PBSYS_CONFIG_BLUETOOTH_TOGGLE
        // Pass the raw current button state to the BT toggle PT
        update_bluetooth_button_wait_state(center_button_currently_pressed);
        #endif

        // General long press shutdown logic
        uint32_t press_duration = pbdrv_clock_get_ms() - center_button_press_start_time;
        bool program_is_running = pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING);

        if (press_duration >= SHUTDOWN_HOLD_DURATION_MS) {
            if (!program_is_running) { // Idle state: long press is shutdown
                pbsys_status_set(PBIO_PYBRICKS_STATUS_SHUTDOWN_REQUEST);
            } else { // Program is running
                // Only trigger shutdown if BT toggle action was NOT taken by a shorter long press
                if (!bluetooth_toggle_action_taken_this_press) {
                    pbsys_status_set(PBIO_PYBRICKS_STATUS_SHUTDOWN_REQUEST);
                }
            }
        }
    } else { // Button is not currently pressed
        if (center_button_press_start_time != 0) {
            // Button was just released
            // update_program_run_button_wait_state will detect the !button_pressed state and act if it was a short click.
            update_program_run_button_wait_state(false); // Signal release for short click processing

            #if PBSYS_CONFIG_BLUETOOTH_TOGGLE
            // Also signal release to BT toggle PT for its state management
            update_bluetooth_button_wait_state(false);
            #endif
            pbsys_status_clear(PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED);
            center_button_press_start_time = 0; // Reset for next press
            // bluetooth_toggle_action_taken_this_press is reset when a new press starts
        }
    }

    // update_program_run_button_wait_state needs to be called even when button is not pressed
    // for its PT_WAIT_UNTIL(!button_pressed) to proceed.
    // However, its main logic for acting on a click is when it transitions through a full cycle.
    // The above calls handle the transitions. If it's continuously not pressed, it just waits.
    // Let's ensure it's polled correctly. The logic above calls it with true/false on transitions.
    // For continuous polling:
    update_program_run_button_wait_state(center_button_currently_pressed);


    #if PBSYS_CONFIG_HMI_NUM_SLOTS
    // Assuming other buttons are not the center button for this logic
    update_left_right_button_wait_state(btn_flags & PBIO_BUTTON_LEFT, btn_flags & PBIO_BUTTON_RIGHT);
    #endif

    pbsys_status_light_poll();
}
