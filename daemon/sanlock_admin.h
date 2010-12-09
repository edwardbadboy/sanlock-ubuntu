#ifndef __SANLOCK_ADMIN_H__
#define __SANLOCK_ADMIN_H__

/*
 * daemon admin/managment
 */

int sanlock_shutdown(void);
int sanlock_status(void);
int sanlock_log_dump(void);
int sanlock_set_host_id(uint32_t our_host_id);

#endif
