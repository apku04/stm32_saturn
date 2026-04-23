/*
 * ina219.h — INA219 current/voltage sensor + charge status
 */

#ifndef INA219_H
#define INA219_H

#include <stdint.h>

typedef enum {
    CHARGE_OFF       = 0,   /* no input power or no battery */
    CHARGE_CHARGING  = 1,   /* actively charging */
    CHARGE_DONE      = 2,   /* charge complete (standby) */
    CHARGE_FAULT     = 3    /* both pins low — fault */
} ChargeStatus;

void          ina219_init(void);
int16_t       ina219_read_shunt_mv(void);   /* shunt voltage in mV (signed) */
uint16_t      ina219_read_bus_mv(void);      /* bus (solar) voltage in mV */
ChargeStatus  charge_get_status(void);
const char   *charge_status_str(ChargeStatus s);

#endif /* INA219_H */
