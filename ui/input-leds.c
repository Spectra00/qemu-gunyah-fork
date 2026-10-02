#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "ui/console.h"

/*
 * Keyboard LED notifiers, kept from the old input-legacy.c: the USB HID
 * keyboard reports guest LED state through kbd_put_ledstate() and the VNC
 * server listens for it (lock-key-sync, LED state pseudo-encoding).
 */


struct QEMUPutLEDEntry {
    QEMUPutLEDEvent *put_led;
    void *opaque;
    QTAILQ_ENTRY(QEMUPutLEDEntry) next;
};

static QTAILQ_HEAD(, QEMUPutLEDEntry) led_handlers =
    QTAILQ_HEAD_INITIALIZER(led_handlers);

QEMUPutLEDEntry *qemu_add_led_event_handler(QEMUPutLEDEvent *func,
                                            void *opaque)
{
    QEMUPutLEDEntry *s;

    s = g_new0(QEMUPutLEDEntry, 1);

    s->put_led = func;
    s->opaque = opaque;
    QTAILQ_INSERT_TAIL(&led_handlers, s, next);
    return s;
}

void qemu_remove_led_event_handler(QEMUPutLEDEntry *entry)
{
    if (entry == NULL)
        return;
    QTAILQ_REMOVE(&led_handlers, entry, next);
    g_free(entry);
}

void kbd_put_ledstate(int ledstate)
{
    QEMUPutLEDEntry *cursor;

    QTAILQ_FOREACH(cursor, &led_handlers, next) {
        cursor->put_led(cursor->opaque, ledstate);
    }
}
