#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>

#include <libevdev/libevdev.h>

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define PTT_UID 1000
#define PTT_PW_DEVICE_NAME "alsa_card.usb-audio-technica_AT2020USB_-00"
#define PTT_PW_REMOTE_NAME "/run/user/1000/pipewire-0"
#define PTT_EV_DEVICE_PATH "/dev/input/by-id/usb-0416_Gaming_Keyboard-event-kbd"
/* See https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h */
#define PTT_EV_KEY_CODE KEY_LEFTCTRL

struct data
{
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;

    struct pw_registry *registry;
    struct spa_hook registry_listener;

    struct pw_device *device;
    struct spa_hook device_listener;

    uint32_t route_index;
    uint32_t route_device;
};

static uint32_t params_filter[] = {SPA_PARAM_Route};

static void device_param(void *object, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
    struct data *data = object;

    {
        const struct spa_pod_prop *prop;

        prop = spa_pod_find_prop(param, NULL, SPA_PARAM_ROUTE_index);
        spa_pod_get_int(&prop->value, &data->route_index);

        prop = spa_pod_find_prop(param, prop, SPA_PARAM_ROUTE_device);
        spa_pod_get_int(&prop->value, &data->route_device);
    }

    pw_main_loop_quit(data->loop);
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .param = device_param,
};

static void registry_event_global(void *_data, uint32_t id,
                                  uint32_t permissions, const char *type,
                                  uint32_t version, const struct spa_dict *props)
{
    struct data *data = _data;
    if (data->device != NULL)
        return;

    if (strcmp(type, PW_TYPE_INTERFACE_Device) == 0)
    {
        const char *device_name = spa_dict_lookup(props, "device.name");
        if (strcmp(device_name, PTT_PW_DEVICE_NAME) != 0)
            return;

        data->device = pw_registry_bind(data->registry,
                                        id, type, PW_VERSION_DEVICE, 0);
        pw_device_add_listener(data->device,
                               &data->device_listener,
                               &device_events, data);

        pw_device_subscribe_params(data->device, params_filter, 1);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

struct roundtrip_data
{
    int pending;
    struct pw_main_loop *loop;
};

static void on_core_done(void *data, uint32_t id, int seq)
{
    struct roundtrip_data *d = data;

    if (id == PW_ID_CORE && seq == d->pending)
        pw_main_loop_quit(d->loop);
}

static void roundtrip(struct pw_core *core, struct pw_main_loop *loop)
{
    static const struct pw_core_events core_events = {
        PW_VERSION_CORE_EVENTS,
        .done = on_core_done,
    };

    struct roundtrip_data d = {.loop = loop};
    struct spa_hook core_listener;

    pw_core_add_listener(core, &core_listener, &core_events, &d);

    d.pending = pw_core_sync(core, PW_ID_CORE, 0);

    pw_main_loop_run(loop);

    spa_hook_remove(&core_listener);
}

void set_mute(struct data *data, bool mute)
{
    uint8_t buffer[4096];
    struct spa_pod_builder b;
    spa_pod_builder_init(&b, buffer, sizeof(buffer));

    struct spa_pod_frame frame_main;
    spa_pod_builder_push_object(&b, &frame_main, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    {
        spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_index, 0);
        spa_pod_builder_int(&b, data->route_index);

        spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_device, 0);
        spa_pod_builder_int(&b, data->route_device);

        spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
        struct spa_pod_frame frame_props;
        spa_pod_builder_push_object(&b, &frame_props, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        {
            spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
            spa_pod_builder_bool(&b, mute);
        }
        spa_pod_builder_pop(&b, &frame_props);
    }
    struct spa_pod *built = spa_pod_builder_pop(&b, &frame_main);

    pw_device_set_param(data->device, SPA_PARAM_Route, 0, built);

    roundtrip(data->core, data->loop);
}

int main(int argc, char *argv[])
{
    int fd = open(PTT_EV_DEVICE_PATH, O_RDONLY);
    if (fd < 0)
    {
        perror("Failed to open device");
        if (getuid() != 0)
            fprintf(stderr, "Fix permissions to %s or run as root\n", PTT_EV_DEVICE_PATH);
        exit(1);
    }

    if (getuid() == 0)
    {
        if (setuid(PTT_UID) != 0)
        {
            fprintf(stderr, "Failed to drop root access: %m\n");
            return -1;
        }
    }

    struct data data;

    spa_zero(data);

    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL)
    {
        fprintf(stderr, "Broken installation: %m\n");
        return -1;
    }
    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (data.context == NULL)
    {
        fprintf(stderr, "Can't create context: %m\n");
        return -1;
    }

    struct pw_properties *properties = pw_properties_new(PW_KEY_REMOTE_NAME, PTT_PW_REMOTE_NAME, NULL);

    data.core = pw_context_connect(data.context, properties, 0);

    if (data.core == NULL)
    {
        fprintf(stderr, "Can't connect: %m\n");
        return -1;
    }

    data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);

    if (data.registry == NULL)
    {
        fprintf(stderr, "Can't get registry: %m\n");
        return -1;
    }

    pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);

    fprintf(stderr, "run loop\n");

    pw_main_loop_run(data.loop);

    fprintf(stderr, "device found\n");

    struct libevdev *dev = NULL;

    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0)
    {
        fprintf(stderr, "Failed to init libevdev (%s)\n", strerror(-rc));
        exit(1);
    }
    fprintf(stderr, "Input device name: \"%s\"\n", libevdev_get_name(dev));
    fprintf(stderr, "Input device ID: bus %#x vendor %#x product %#x\n",
            libevdev_get_id_bustype(dev),
            libevdev_get_id_vendor(dev),
            libevdev_get_id_product(dev));

    if (!libevdev_has_event_code(dev, EV_KEY, PTT_EV_KEY_CODE))
    {
        fprintf(stderr, "This device is not capable of sending this key code\n");
        exit(1);
    }

    set_mute(&data, true);

    do
    {
        struct input_event ev;

        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS)
            continue;

        if (ev.type == EV_KEY && ev.code == PTT_EV_KEY_CODE && ev.value != 2)
        {
            set_mute(&data, !(bool)ev.value);
        }
    } while (rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == -EAGAIN);

    libevdev_free(dev);
    close(fd);

    pw_proxy_destroy((struct pw_proxy *)data.device);
    pw_proxy_destroy((struct pw_proxy *)data.registry);
    pw_core_disconnect(data.core);
    pw_context_destroy(data.context);
    pw_main_loop_destroy(data.loop);

    return 0;
}
