#pragma once

#include <stdbool.h>
#include <stddef.h>

// Initialize and start the WebSocket connection
void tosu_init(void);

// Stop WebSocket thread & cleanup
void tosu_shutdown(void);

// Call from main loop to get current absolute activation state
bool tosu_get_absolute_state(void);
