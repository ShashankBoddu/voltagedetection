/*
 * hc4067.c - Library for HC4067 Multiplexer
 */

#include "hc4067.h"
#include <zephyr/drivers/gpio.h>

/* The devicetree node identifier for the "range_selIC" alias. */
#define RANGE_SEL_IC0_NODE DT_NODELABEL(range_selic0)
#define RANGE_SEL_IC1_NODE DT_NODELABEL(range_selic1)
#define RANGE_SEL_IC2_NODE DT_NODELABEL(range_selic2)
#define RANGE_SEL_IC3_NODE DT_NODELABEL(range_selic3)

static const struct gpio_dt_spec s0 = GPIO_DT_SPEC_GET(RANGE_SEL_IC0_NODE, gpios);
static const struct gpio_dt_spec s1 = GPIO_DT_SPEC_GET(RANGE_SEL_IC1_NODE, gpios);
static const struct gpio_dt_spec s2 = GPIO_DT_SPEC_GET(RANGE_SEL_IC2_NODE, gpios);
static const struct gpio_dt_spec s3 = GPIO_DT_SPEC_GET(RANGE_SEL_IC3_NODE, gpios);

int hc4067_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&s0) || !gpio_is_ready_dt(&s1) ||
	    !gpio_is_ready_dt(&s2) || !gpio_is_ready_dt(&s3)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&s0, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) return ret;

	ret = gpio_pin_configure_dt(&s1, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) return ret;

	ret = gpio_pin_configure_dt(&s2, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) return ret;

	ret = gpio_pin_configure_dt(&s3, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) return ret;

	return 0;
}

void hc4067_set_channel(uint8_t channel)
{
	gpio_pin_set_dt(&s0, (channel & 0x01) ? 1 : 0);
	gpio_pin_set_dt(&s1, (channel & 0x02) ? 1 : 0);
	gpio_pin_set_dt(&s2, (channel & 0x04) ? 1 : 0);
	gpio_pin_set_dt(&s3, (channel & 0x08) ? 1 : 0);
}
