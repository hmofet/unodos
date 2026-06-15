/* ===========================================================================
 * UnoDOS/PS2 embedded module images.  The 11 app modules' relocatable .uno
 * objects, embedded into the ELF via bin2c (Makefile -> build/mod_images.c),
 * so the EE port can WRITE them to mc0:/UnoDOS/Apps/appNN.uno on first boot.
 * After that the overlay loader (ee_modload.c) reads them back OFF THE CARD and
 * relocates - a genuine storage round-trip with no external card preparation.
 * ===========================================================================
 */
#ifndef UNO_MOD_IMAGES_H
#define UNO_MOD_IMAGES_H

typedef struct { const unsigned char *data; long len; } UnoModImage;

/* indexed by app id (APP_SYSINFO..APP_THEME) */
extern const UnoModImage gUnoModImages[11];

#endif /* UNO_MOD_IMAGES_H */
