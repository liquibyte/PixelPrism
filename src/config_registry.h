/**
 * @file config_registry.h
 * @brief Config section registry API for PixelPrism
 *
 * Exposes the lightweight registry used by config.c to keep widget-specific
 * configuration handlers modular. Each handler registers itself with a unique
 * section name and provides callbacks for default initialization, parsing, and
 * writing.
 */

#ifndef CONFIG_REGISTRY_H_
#define CONFIG_REGISTRY_H_

#include <stdio.h>

struct ConfigSectionHandler;
typedef struct ConfigSectionHandler ConfigSectionHandler;
struct PixelPrismConfig;

struct ConfigSectionHandler {
    const char *section;
    void (*init_defaults)(struct PixelPrismConfig *cfg);
    int (*parse)(struct PixelPrismConfig *cfg, const char *key, const char *value);
    void (*write)(FILE *f, const struct PixelPrismConfig *cfg);
};

void config_registry_reset(void);
void config_registry_register(const ConfigSectionHandler *handler);
const ConfigSectionHandler *config_registry_find(const char *section);
void config_registry_for_each(void (*callback)(const ConfigSectionHandler *handler, void *user_data), void *user_data);

#endif /* CONFIG_REGISTRY_H_ */
