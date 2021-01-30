/*
 * bsp.h
 *
 *  Created on: Jan 6, 2021
 *      Author: AFont
 */

#ifndef MAIN_BSP_H_
#define MAIN_BSP_H_

#include <driver/gpio.h>

#define GPIO_BSP_TP_0 21
#define GPIO_BSP_TP_1 22
#define GPIO_BSP_TP_2 23

#define BSP_TP_CLEAR_0() gpio_set_level(GPIO_BSP_TP_0, 0)
#define BSP_TP_CLEAR_1() gpio_set_level(GPIO_BSP_TP_1, 0)
#define BSP_TP_CLEAR_2() gpio_set_level(GPIO_BSP_TP_2, 0)

#define BSP_TP_SET_0() gpio_set_level(GPIO_BSP_TP_0, 1)
#define BSP_TP_SET_1() gpio_set_level(GPIO_BSP_TP_1, 1)
#define BSP_TP_SET_2() gpio_set_level(GPIO_BSP_TP_2, 1)

#define BSP_TP_SET_NUM_0()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 0); \
        gpio_set_level(GPIO_BSP_TP_1, 0); \
        gpio_set_level(GPIO_BSP_TP_2, 0); \
    }

#define BSP_TP_SET_NUM_1()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 1); \
        gpio_set_level(GPIO_BSP_TP_1, 0); \
        gpio_set_level(GPIO_BSP_TP_2, 0); \
    }

#define BSP_TP_SET_NUM_2()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 0); \
        gpio_set_level(GPIO_BSP_TP_1, 1); \
        gpio_set_level(GPIO_BSP_TP_2, 0); \
    }

#define BSP_TP_SET_NUM_3()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 1); \
        gpio_set_level(GPIO_BSP_TP_1, 1); \
        gpio_set_level(GPIO_BSP_TP_2, 0); \
    }

#define BSP_TP_SET_NUM_4()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 0); \
        gpio_set_level(GPIO_BSP_TP_1, 0); \
        gpio_set_level(GPIO_BSP_TP_2, 1); \
    }

#define BSP_TP_SET_NUM_5()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 1); \
        gpio_set_level(GPIO_BSP_TP_1, 0); \
        gpio_set_level(GPIO_BSP_TP_2, 1); \
    }

#define BSP_TP_SET_NUM_6()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 0); \
        gpio_set_level(GPIO_BSP_TP_1, 1); \
        gpio_set_level(GPIO_BSP_TP_2, 1); \
    }

#define BSP_TP_SET_NUM_7()                \
    {                                     \
        gpio_set_level(GPIO_BSP_TP_0, 1); \
        gpio_set_level(GPIO_BSP_TP_1, 1); \
        gpio_set_level(GPIO_BSP_TP_2, 1); \
    }

#define BSP_TP_CLEAR_ALL() BSP_TP_SET_NUM_0()
#define BSP_TP_SET_ALL()   BSP_TP_SET_NUM_7()

#define BSP_TP_INIT()                                                                          \
    {                                                                                          \
        gpio_config_t gpioConfig = {                                                           \
            .pin_bit_mask =                                                                    \
                ((1ULL << GPIO_BSP_TP_0) | (1ULL << GPIO_BSP_TP_1) | (1ULL << GPIO_BSP_TP_2)), \
            .mode = GPIO_MODE_OUTPUT,                                                          \
            .pull_up_en = GPIO_PULLUP_DISABLE,                                                 \
            .pull_down_en = GPIO_PULLDOWN_DISABLE,                                             \
            .intr_type = GPIO_INTR_DISABLE,                                                    \
        };                                                                                     \
        gpio_config(&gpioConfig);                                                              \
        BSP_TP_CLEAR_ALL();                                                                    \
    }

#endif /* MAIN_BSP_H_ */
