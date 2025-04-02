#define DT_DRV_COMPAT zmk_behavior_swapper

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zmk/behavior.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD 10
#define ZMK_BHV_SWAPPER_POSITION_FREE UINT32_MAX

struct behavior_swapper_config
{
    uint32_t release_after_ms;
    struct zmk_behavior_binding behavior;
};

struct behavior_swapper_data
{
    bool active;
    uint32_t position;
    uint32_t cmdish;
    uint32_t tabish;
    const struct behavior_swapper_config *config;
    // timer data.
    bool timer_started;
    bool timer_cancelled;
    int64_t release_at;
    struct k_work_delayable release_timer;
};
struct behavior_swapper_data active_swapper_keys[CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD] = {};

static int new_swapper(uint32_t position, uint32_t cmdish, uint32_t tabish,
                       struct behavior_swapper_config *config,
                       struct behavior_swapper_data **swapper)
{
    for (int i = 0; i < CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD; i++)
    {
        struct behavior_swapper_data *const ref_swapper = &active_swapper_keys[i];
        if (ref_swapper->position == ZMK_BHV_SWAPPER_POSITION_FREE)
        {
            ref_swapper->active = false;
            ref_swapper->position = position;
            ref_swapper->cmdish = cmdish;
            ref_swapper->tabish = tabish;
            ref_swapper->config = config;
            ref_swapper->timer_started = false;
            ref_swapper->timer_cancelled = false;
            ref_swapper->release_at = 0;
            *swapper = ref_swapper;
            return 0;
        }
    }
    return -ENOMEM;
}

static struct behavior_swapper_data *find_swapper(uint32_t position, uint32_t cmdish,
                                                  uint32_t tabish)
{
    for (int i = 0; i < CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD; i++)
    {
        struct behavior_swapper_data *swapper = &active_swapper_keys[i];
        if (swapper->position == position && swapper->cmdish == cmdish)
        {
            return &active_swapper_keys[i];
        }
    }
    return NULL;
}
static void clear_swapper_key(struct behavior_swapper_data *swapper)
{
    swapper->position = ZMK_BHV_SWAPPER_POSITION_FREE;
    swapper->active = false;
    swapper->timer_started = false;
    swapper->timer_cancelled = false;
}

static inline int invoke_swapper_behavior(struct behavior_swapper_data *swapper, int64_t timestamp,
                                          uint32_t keycode, bool pressed)
{

    struct zmk_behavior_binding binding = {
        .behavior_dev = swapper->config->behavior.behavior_dev,
        .param1 = keycode,
    };
    struct zmk_behavior_binding_event event = {
        .position = swapper->position,
        .timestamp = timestamp,
    };

    return zmk_behavior_invoke_binding(&binding, event, pressed);
}
static inline int press_cmdish(struct behavior_swapper_data *swapper, int64_t timestamp)
{
    return invoke_swapper_behavior(swapper, timestamp, swapper->cmdish, true);
}
static inline int release_cmdish(struct behavior_swapper_data *swapper, int64_t timestamp)
{
    return invoke_swapper_behavior(swapper, timestamp, swapper->cmdish, false);
}
static inline int press_tabish(struct behavior_swapper_data *swapper, int64_t timestamp)
{
    return invoke_swapper_behavior(swapper, timestamp, swapper->tabish, true);
}
static inline int release_tabish(struct behavior_swapper_data *swapper, int64_t timestamp)
{
    return invoke_swapper_behavior(swapper, timestamp, swapper->tabish, false);
}

static int stop_timer(struct behavior_swapper_data *swapper)
{
    LOG_DBG("stopping timer... position %d", swapper->position);
    int timer_cancel_result = k_work_cancel_delayable(&swapper->release_timer);
    if (timer_cancel_result == -EINPROGRESS)
    {
        // too late to cancel, we'll let the timer handler clear up.
        swapper->timer_cancelled = true;
    }
    else if (timer_cancel_result >= 0)
    {
        swapper->timer_started = false;
    }

    return timer_cancel_result;
}
static void start_timer(struct behavior_swapper_data *swapper,
                        struct zmk_behavior_binding_event event)
{
    LOG_DBG("starting timer... position %d", swapper->position);
    swapper->timer_started = true;
    swapper->release_at = event.timestamp + swapper->config->release_after_ms;
    // adjust timer in case this behavior was queued by a hold-tap
    int32_t ms_left = swapper->release_at - k_uptime_get();
    if (ms_left > 0)
    {
        k_work_schedule(&swapper->release_timer, K_MSEC(ms_left));
    }
}

static int on_swapper_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    uint32_t cmdish = binding->param1;
    uint32_t tabish = binding->param2;
    LOG_DBG("cmdish 0x%02X, tabish 0x%02X, position %d", cmdish, tabish, event.position);
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);

    struct behavior_swapper_data *swapper = find_swapper(event.position, cmdish, tabish);
    if (swapper == NULL)
    {
        if (new_swapper(event.position, cmdish, tabish,
                        (struct behavior_swapper_config *)dev->config, &swapper) == -ENOMEM)
        {
            LOG_ERR("Unable to create new swapper. Insufficient space in swapper_states[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new swapper", event.position);
        swapper->active = true;
        press_cmdish(swapper, event.timestamp);
        LOG_DBG("swapper activated, cmdish 0x%02X, position 0x%02X", cmdish, event.position);
    }
    else
    {
        if (swapper->timer_started)
        {
            stop_timer(swapper);
        }
        if (!swapper->active)
        {
            swapper->active = true;
            press_cmdish(swapper, event.timestamp);
            LOG_DBG("swapper reactivated, position %d", swapper->position);
        }
    }
    press_tabish(swapper, event.timestamp);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_swapper_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event)
{
    uint32_t cmdish = binding->param1;
    uint32_t tabish = binding->param2;
    struct behavior_swapper_data *swapper = find_swapper(event.position, cmdish, tabish);
    if (swapper == NULL)
    {
        LOG_DBG("swapper was deactivated by another keytap");
        return ZMK_BEHAVIOR_OPAQUE;
    }
    release_tabish(swapper, event.timestamp);
    start_timer(swapper, event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int swapper_position_state_changed_listener(const zmk_event_t *eh)
{
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_behavior_binding *binding =
        zmk_keymap_get_layer_binding_at_idx(zmk_keymap_highest_layer_active(), ev->position);

    for (size_t i = 0; i < CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD; i++)
    {
        struct behavior_swapper_data *swapper = &active_swapper_keys[i];
        if (swapper->active && swapper->cmdish != binding->param1)
        {
            release_cmdish(swapper, ev->timestamp);
            LOG_DBG("swapper deactivated, cmdish 0x%02X, position 0x%02X", swapper->cmdish,
                    swapper->position);
            if (swapper->active)
            {
                swapper->active = false;
            }
            else
            {
                clear_swapper_key(swapper);
            }
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

void behavior_swapper_timer_handler(struct k_work *item)
{
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct behavior_swapper_data *swapper =
        CONTAINER_OF(d_work, struct behavior_swapper_data, release_timer);

    if (!swapper->timer_cancelled && swapper->active && swapper->timer_started)
    {
        release_cmdish(swapper, k_uptime_get());
        LOG_DBG("swapper deactivated, cmdish 0x%02X, position 0x%02X", swapper->cmdish,
                swapper->position);
        clear_swapper_key(swapper);
    }
    else if (swapper->timer_cancelled)
    {
        swapper->timer_cancelled = false;
    }
    else if (!swapper->active)
    {
        LOG_DBG("swapper was deactivated by position state change, position %d, active %d",
                swapper->position, swapper->active);
        clear_swapper_key(swapper);
    }
}

static int behavior_swapper_init(const struct device *dev)
{
    static bool init_first_run = true;
    if (init_first_run)
    {
        for (int i = 0; i < CONFIG_ZMK_BEHAVIOR_SWAPPER_MAX_HELD; i++)
        {
            active_swapper_keys[i].position = ZMK_BHV_SWAPPER_POSITION_FREE;
            active_swapper_keys[i].active = false;
            k_work_init_delayable(&active_swapper_keys[i].release_timer,
                                  behavior_swapper_timer_handler);
        }
    }
    init_first_run = false;
    return 0;
}

static const struct behavior_driver_api behavior_swapper_driver_api = {
    .binding_pressed = on_swapper_binding_pressed,
    .binding_released = on_swapper_binding_released,
};

ZMK_LISTENER(behavior_swapper, swapper_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_swapper, zmk_position_state_changed);

#define SWP_INST(n)                                                                             \
    static const struct behavior_swapper_config behavior_swapper_config_##n = {                 \
        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                              \
        .release_after_ms = DT_INST_PROP(n, release_after_ms),                                  \
    };                                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_swapper_init, NULL, NULL, &behavior_swapper_config_##n, \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                   \
                            &behavior_swapper_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SWP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
