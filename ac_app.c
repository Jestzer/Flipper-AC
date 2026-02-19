#include <furi.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <input/input.h>
#include "infrared_signal.h"

// Timing constants.
static const uint32_t one_second_interval = 1000; // 1 second in milliseconds
static const uint32_t one_minute_interval = 60000; // 1 minute in milliseconds
static const uint32_t one_hour_interval = 3600000; // 1 hour in milliseconds
static const uint32_t three_hour_interval = 10800000; // 3 hours in milliseconds

// Global variables.
static const char* ac_on_text = "The A/C should be on.";
static const char* ac_off_text = "The A/C should be off.";
static bool ac_is_on = false;
static uint32_t next_signal_interval = one_hour_interval;
static uint32_t remaining_time = one_hour_interval; // Initial remaining time in milliseconds

// Infrared signals to be used.
static const uint32_t ir_address_1 = 0x00006F98; // The A/C itself
static const uint32_t ir_command_1 = 0x0000E619; // Power button
static const uint32_t ir_command_2 = 0x0000F708; // Mode button

// Turn-on sequence state machine.
typedef enum {
    AcSequenceIdle,
    AcSequenceTurnOnStep1, // Power sent, waiting to send first Mode
    AcSequenceTurnOnStep2, // First Mode sent, waiting to send second Mode
} AcSequenceState;

static AcSequenceState sequence_state = AcSequenceIdle;

// Timers. Putting them here to avoid NULL pointer dereferences.
static FuriTimer* signal_timer = NULL;
static FuriTimer* countdown_timer = NULL;
static FuriTimer* sequence_timer = NULL;

// Function to handle GUI events.
static void ac_app_render_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    // Display the appropriate text.
    canvas_draw_str_aligned(
        canvas, 64, 32, AlignCenter, AlignCenter, ac_is_on ? ac_on_text : ac_off_text);

    // Calculate the remaining minutes.
    uint32_t remaining_minutes = remaining_time / one_minute_interval;
    char countdown_text[32];
    if(remaining_minutes == 0) {
        snprintf(countdown_text, sizeof(countdown_text), "Sending signal soon...");
    } else if(remaining_minutes == 1) {
        snprintf(countdown_text, sizeof(countdown_text), "Next signal in 1 min.");
    } else {
        snprintf(
            countdown_text, sizeof(countdown_text), "Next signal in %lu mins.", remaining_minutes);
    }

    // Display the countdown text on-screen.
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, countdown_text);
}

// Function to send the infrared signal.
static void send_ir_signal(uint32_t address, uint32_t command) {
    InfraredSignal* signal = infrared_signal_alloc();
    InfraredMessage message = {
        .protocol = InfraredProtocolNECext,
        .address = address,
        .command = command,
    };
    infrared_signal_set_message(signal, &message);
    infrared_signal_transmit(signal);
    infrared_signal_free(signal);
    FURI_LOG_I(
        "ir_tx",
        "Sent infrared signal: address=0x%08lX, command=0x%08lX",
        address,
        command);
}

// Sequence timer callback to complete the turn-on steps without blocking.
static void sequence_step_callback(void* ctx) {
    ViewPort* view_port = (ViewPort*)ctx;

    if(sequence_state == AcSequenceTurnOnStep1) {
        send_ir_signal(ir_address_1, ir_command_2);
        sequence_state = AcSequenceTurnOnStep2;
        furi_timer_start(sequence_timer, one_second_interval);
    } else if(sequence_state == AcSequenceTurnOnStep2) {
        send_ir_signal(ir_address_1, ir_command_2);
        sequence_state = AcSequenceIdle;
        ac_is_on = true;
        next_signal_interval = one_hour_interval;
        remaining_time = next_signal_interval;

        // Schedule the next signal.
        furi_timer_stop(signal_timer);
        furi_timer_start(signal_timer, next_signal_interval);

        view_port_update(view_port);
        FURI_LOG_I("ac_app", "The A/C should be on.");
    }
}

// Function to actually send signals and update text based on the current state.
static void send_signals_and_update_text(void* ctx) {
    ViewPort* view_port = (ViewPort*)ctx;

    if(ac_is_on) {
        // Send signal to turn off the A/C and update the text.
        send_ir_signal(ir_address_1, ir_command_1);
        ac_is_on = false;
        next_signal_interval = three_hour_interval;
        remaining_time = next_signal_interval;

        // Schedule the next signal based on the current state.
        furi_timer_stop(signal_timer);
        furi_timer_start(signal_timer, next_signal_interval);

        view_port_update(view_port);
        FURI_LOG_I("ac_app", "The A/C should be off.");
    } else {
        // Start the turn-on sequence: send Power, then Mode twice with delays.
        send_ir_signal(ir_address_1, ir_command_1);
        sequence_state = AcSequenceTurnOnStep1;
        furi_timer_start(sequence_timer, one_second_interval);

        // Update the text on the screen.
        view_port_update(view_port);
    }
}

// Timer callback to update the countdown displayed on-screen.
static void update_countdown(void* ctx) {
    ViewPort* view_port = (ViewPort*)ctx;

    if(remaining_time >= one_minute_interval) {
        remaining_time -= one_minute_interval; // Decrease by 1 minute at a time.
    } else {
        remaining_time = 0;
    }

    // Log the remaining time.
    uint32_t remaining_minutes = remaining_time / one_minute_interval;
    if(remaining_minutes == 1) {
        FURI_LOG_I("countdown", "Time remaining until next signal: 1 minute");
    } else {
        FURI_LOG_I(
            "countdown", "Time remaining until next signal: %lu minutes", remaining_minutes);
    }

    // Update the text on the screen.
    view_port_update(view_port);

    // Schedule the next countdown update in one minute.
    furi_timer_stop(countdown_timer);
    furi_timer_start(countdown_timer, one_minute_interval);
}

// Handle input.
static void ac_app_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    if(input_event->key == InputKeyBack && input_event->type == InputTypeShort) {
        FURI_LOG_I("ac_app", "Received input to close the application.");
        FuriMessageQueue* event_queue = (FuriMessageQueue*)ctx;
        InputEvent exit_event = {.type = InputTypeShort, .key = InputKeyBack};
        furi_message_queue_put(event_queue, &exit_event, FuriWaitForever);
    }
}

int32_t ac_app_app(void* p) { // The actual sequence of events.
    UNUSED(p);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    FURI_LOG_I("ac_app", "The app started.");

    // Creating and configuring a ViewPort.
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, ac_app_render_callback, view_port);
    view_port_input_callback_set(view_port, ac_app_input_callback, event_queue);

    // Get the default GUI instance and add the ViewPort to it.
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Initialize the timers.
    signal_timer = furi_timer_alloc(send_signals_and_update_text, FuriTimerTypeOnce, view_port);
    countdown_timer = furi_timer_alloc(update_countdown, FuriTimerTypeOnce, view_port);
    sequence_timer = furi_timer_alloc(sequence_step_callback, FuriTimerTypeOnce, view_port);

    // Start sending signals.
    send_signals_and_update_text(view_port);

    // Schedule the first countdown update in 1 minute.
    furi_timer_start(countdown_timer, one_minute_interval);

    // Run the input event loop so the app doesn't stop until we say so.
    InputEvent event;
    while(true) {
        if(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.key == InputKeyBack) {
                FURI_LOG_I("ac_app", "Closing the application!");
                break;
            }
        }
    }

    // Cleanup.
    if(signal_timer) {
        furi_timer_stop(signal_timer);
        furi_timer_free(signal_timer);
    }
    if(countdown_timer) {
        furi_timer_stop(countdown_timer);
        furi_timer_free(countdown_timer);
    }
    if(sequence_timer) {
        furi_timer_stop(sequence_timer);
        furi_timer_free(sequence_timer);
    }
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(event_queue);

    return 0;
}
