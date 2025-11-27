/* config_registry.c - Config Section Registry Implementation
 *
 * Centralized registry that keeps widget config handlers decoupled from the
 * parser. Each handler self-describes the section it manages and provides
 * callbacks for initializing defaults, parsing key/value pairs, and emitting
 * configuration blocks.
 *
 * Responsibilities:
 * - Maintain a bounded list of registered section handlers
 * - Provide efficient lookup by section name
 * - Iterate handlers for bulk operations (init/write)
 */

#include "config_registry.h"
#include <string.h>

#define MAX_CONFIG_HANDLERS 32

/**
 * Array of registered config section handlers.
 *
 * The array is bounded to prevent excessive memory usage.
 */
static const ConfigSectionHandler *registered_handlers[MAX_CONFIG_HANDLERS];

/**
 * Number of registered config section handlers.
 */
static size_t registered_count = 0;

/**
 * Reset the config registry by clearing all registered handlers.
 */
void config_registry_reset(void) {
    registered_count = 0;
}

/**
 * Register a config section handler.
 *
 * @param handler Config section handler to register.
 *
 * @note If the handler is already registered or has a duplicate section name,
 *       the registration is ignored.
 */
void config_registry_register(const ConfigSectionHandler *handler) {
    if (!handler || !handler->section) {
        return;
    }
    for (size_t i = 0; i < registered_count; ++i) {
        if (registered_handlers[i] == handler ||
            strcmp(registered_handlers[i]->section, handler->section) == 0) {
            return; // already registered
        }
    }
    if (registered_count < MAX_CONFIG_HANDLERS) {
        registered_handlers[registered_count++] = handler;
    }
}

/**
 * Find a registered config section handler by section name.
 *
 * @param section Section name to search for.
 *
 * @return Registered config section handler, or NULL if not found.
 */
const ConfigSectionHandler *config_registry_find(const char *section) {
    if (!section) {
        return NULL;
    }
    for (size_t i = 0; i < registered_count; ++i) {
        if (strcmp(registered_handlers[i]->section, section) == 0) {
            return registered_handlers[i];
        }
    }
    return NULL;
}

/**
 * Iterate over all registered config section handlers.
 *
 * @param callback Callback function to invoke for each handler.
 * @param user_data User data to pass to the callback function.
 */
void config_registry_for_each(void (*callback)(const ConfigSectionHandler *handler, void *user_data), void *user_data) {
    if (!callback) {
        return;
    }
    for (size_t i = 0; i < registered_count; ++i) {
        callback(registered_handlers[i], user_data);
    }
}
