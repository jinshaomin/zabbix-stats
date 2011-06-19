/*
** ZABBIX
** Copyright (C) 2000-2005 SIA Zabbix
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**/

#ifndef ZABBIX_CHECKS_INTERNAL_H
#define ZABBIX_CHECKS_INTERNAL_H

#include "common.h"
#include "dbcache.h"
#include "sysinfo.h"
#include "comms.h"

extern int	CONFIG_SERVER_STARTUP_TIME;

int	get_value_internal(DC_ITEM *item, AGENT_RESULT *result);
int	send_perf_stats(zbx_sock_t *sock);

#endif
