
#include "qemu/osdep.h"

#include "monitor-internal.h"
#include "qemu-version.h"
#include "qapi/compat-policy.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qobject-input-visitor.h"

static bool qmp_caps_accept(MonitorQMP *mon, QMPCapabilityList *list,
                            Error **errp)
{
    GString *unavailable = NULL;
    bool capab[QMP_CAPABILITY__MAX];

    memset(capab, 0, sizeof(capab));

    for (; list; list = list->next) {
        if (!mon->capab_offered[list->value]) {
            if (!unavailable) {
                unavailable = g_string_new(QMPCapability_str(list->value));
            } else {
                g_string_append_printf(unavailable, ", %s",
                                      QMPCapability_str(list->value));
            }
        }
        capab[list->value] = true;
    }

    if (unavailable) {
        error_setg(errp, "Capability %s not available", unavailable->str);
        g_string_free(unavailable, true);
        return false;
    }

    memcpy(mon->capab, capab, sizeof(capab));
    return true;
}

void qmp_qmp_capabilities(bool has_enable, QMPCapabilityList *enable,
                          Error **errp)
{
    Monitor *cur_mon = monitor_cur();
    MonitorQMP *mon;

    assert(monitor_is_qmp(cur_mon));
    mon = container_of(cur_mon, MonitorQMP, common);

    if (mon->commands == &qmp_commands) {
        error_set(errp, ERROR_CLASS_COMMAND_NOT_FOUND,
                  "Capabilities negotiation is already complete, command "
                  "ignored");
        return;
    }

    if (!qmp_caps_accept(mon, enable, errp)) {
        return;
    }

    mon->commands = &qmp_commands;
}

VersionInfo *qmp_query_version(Error **errp)
{
    VersionInfo *info = g_new0(VersionInfo, 1);

    info->qemu = g_new0(VersionTriple, 1);
    info->qemu->major = QEMU_VERSION_MAJOR;
    info->qemu->minor = QEMU_VERSION_MINOR;
    info->qemu->micro = QEMU_VERSION_MICRO;
    info->package = g_strdup(QEMU_PKGVERSION);

    return info;
}

static void query_commands_cb(const QmpCommand *cmd, void *opaque)
{
    CommandInfo *info;
    CommandInfoList **list = opaque;

    if (!cmd->enabled) {
        return;
    }

    info = g_malloc0(sizeof(*info));
    info->name = g_strdup(cmd->name);
    QAPI_LIST_PREPEND(*list, info);
}

CommandInfoList *qmp_query_commands(Error **errp)
{
    CommandInfoList *list = NULL;
    Monitor *cur_mon = monitor_cur();
    MonitorQMP *mon;

    assert(monitor_is_qmp(cur_mon));
    mon = container_of(cur_mon, MonitorQMP, common);

    qmp_for_each_command(mon->commands, query_commands_cb, &list);

    return list;
}
