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

#include "common.h"
#include "active.h"

#include "cfg.h"
#include "log.h"
#include "sysinfo.h"
#include "logfiles.h"
#if defined (_WINDOWS)
#	include "eventlog.h"
#endif	/* _WINDOWS */
#include "comms.h"
#include "threads.h"
#include "zbxjson.h"

#if defined(ZABBIX_SERVICE)
#	include "service.h"
#elif defined(ZABBIX_DAEMON)	/* ZABBIX_SERVICE */
#	include "daemon.h"
#endif	/* ZABBIX_DAEMON */

static ZBX_ACTIVE_METRIC *active_metrics = NULL;
static ZBX_ACTIVE_BUFFER buffer;
static ZBX_REGEXP	*regexps = NULL;
static int		regexps_alloc = 0, regexps_num = 0;

static void	init_active_metrics()
{
	size_t	sz;

	zabbix_log(LOG_LEVEL_DEBUG, "In init_active_metrics()");

	active_metrics = zbx_malloc(active_metrics, sizeof(ZBX_ACTIVE_METRIC));
	active_metrics->key = NULL;

	if (NULL == buffer.data)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "Buffer: first allocation for %d elements",
				CONFIG_BUFFER_SIZE);
		sz = CONFIG_BUFFER_SIZE * sizeof(ZBX_ACTIVE_BUFFER_ELEMENT);
		buffer.data = zbx_malloc(buffer.data, sz);
		memset(buffer.data, 0, sz);
		buffer.count = 0;
		buffer.pcount = 0;
		buffer.lastsent = (int)time(NULL);
	}
}

static void	disable_all_metrics()
{
	int	i;

	zabbix_log(LOG_LEVEL_DEBUG, "In disable_all_metrics()");

	for (i = 0; NULL != active_metrics[i].key; i++)
		active_metrics[i].status = ITEM_STATUS_NOTSUPPORTED;
}

static void	free_active_metrics()
{
	int	i;

	zabbix_log(LOG_LEVEL_DEBUG, "In free_active_metrics()");

	for (i = 0; NULL != active_metrics[i].key; i++)
		zbx_free(active_metrics[i].key);

	zbx_free(active_metrics);

	clean_regexps_ex(regexps, &regexps_num);

	zbx_free(regexps);
}

static int	get_min_nextcheck()
{
	int	i, min = -1;

	zabbix_log(LOG_LEVEL_DEBUG, "In get_min_nextcheck()");

	for (i = 0; NULL != active_metrics[i].key; i++)
	{
		if (ITEM_STATUS_ACTIVE != active_metrics[i].status)
			continue;

		if (active_metrics[i].nextcheck < min || (-1) == min)
			min = active_metrics[i].nextcheck;
	}

	if ((-1) == min)
		return FAIL;

	return min;
}

static void	add_check(const char *key, const char *key_orig, int refresh, long lastlogsize, int mtime)
{
	int	i;

	zabbix_log(LOG_LEVEL_DEBUG, "In add_check('%s', %i, %li, %i)", key, refresh, lastlogsize, mtime);

	for (i = 0; NULL != active_metrics[i].key; i++)
	{
		if (0 != strcmp(active_metrics[i].key_orig, key_orig))
			continue;

		if (0 != strcmp(active_metrics[i].key, key))
		{
			zbx_free(active_metrics[i].key);
			active_metrics[i].key		= strdup(key);
			active_metrics[i].lastlogsize	= lastlogsize;
			active_metrics[i].mtime		= mtime;
		}

		/* replace metric */
		if (active_metrics[i].refresh != refresh)
		{
			active_metrics[i].nextcheck = 0;
			active_metrics[i].refresh = refresh;
		}
		active_metrics[i].status = ITEM_STATUS_ACTIVE;

		return;
	}

	/* add new metric */
	active_metrics[i].key		= strdup(key);
	active_metrics[i].key_orig	= strdup(key_orig);
	active_metrics[i].refresh	= refresh;
	active_metrics[i].nextcheck	= 0;
	active_metrics[i].status	= ITEM_STATUS_ACTIVE;
	active_metrics[i].lastlogsize	= lastlogsize;
	active_metrics[i].mtime		= mtime;

	/* move to the last metric */
	i++;

	/* allocate memory for last metric */
	active_metrics	= zbx_realloc(active_metrics, (i + 1) * sizeof(ZBX_ACTIVE_METRIC));

	/* initialize last metric */
	active_metrics[i].key = NULL;
}

/******************************************************************************
 *                                                                            *
 * Function: parse_list_of_checks                                             *
 *                                                                            *
 * Purpose: Parse list of active checks received from server                  *
 *                                                                            *
 * Parameters: str - NULL terminated string received from server              *
 *                                                                            *
 * Return value: returns SUCCEED on successful parsing,                       *
 *               FAIL on an incorrect format of string                        *
 *                                                                            *
 * Author: Eugene Grigorjev, Alexei Vladishev (new json protocol)             *
 *                                                                            *
 * Comments:                                                                  *
 *    String represented as "ZBX_EOF" termination list                        *
 *    With '\n' delimeter between elements.                                   *
 *    Each element represented as:                                            *
 *           <key>:<refresh time>:<last log size>:<modification time>         *
 *                                                                            *
 ******************************************************************************/
static int	parse_list_of_checks(char *str)
{
	const char		*p;
	char			name[MAX_STRING_LEN], key_orig[MAX_STRING_LEN], expression[MAX_STRING_LEN], tmp[MAX_STRING_LEN],
				exp_delimiter;
	int			delay, lastlogsize, mtime, expression_type, case_sensitive;
	struct zbx_json_parse	jp;
	struct zbx_json_parse	jp_data, jp_row;

	zabbix_log(LOG_LEVEL_DEBUG, "In parse_list_of_checks()");

	disable_all_metrics();

	if (SUCCEED != zbx_json_open(str, &jp))
		goto json_error;

	if (SUCCEED != zbx_json_value_by_name(&jp, ZBX_PROTO_TAG_RESPONSE, tmp, sizeof(tmp)))
		goto json_error;

	if (0 != strcmp(tmp, ZBX_PROTO_VALUE_SUCCESS))
	{
		if (SUCCEED == zbx_json_value_by_name(&jp, ZBX_PROTO_TAG_INFO, tmp, sizeof(tmp)))
			zabbix_log(LOG_LEVEL_WARNING, "No active checks on server: %s",
					tmp);
		else
			zabbix_log(LOG_LEVEL_WARNING, "No active checks on server");
		return FAIL;
	}

	if (SUCCEED != zbx_json_brackets_by_name(&jp, ZBX_PROTO_TAG_DATA, &jp_data))
		goto json_error;

 	p = NULL;
	while (NULL != (p = zbx_json_next(&jp_data, p)))
	{
/* {"data":[{"key":"system.cpu.num",...,...},{...},...]}
 *          ^------------------------------^
 */ 		if (SUCCEED != zbx_json_brackets_open(p, &jp_row))
			goto json_error;

		if (SUCCEED != zbx_json_value_by_name(&jp_row, ZBX_PROTO_TAG_KEY, name, sizeof(name)) || '\0' == *name)
		{
			zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",
					ZBX_PROTO_TAG_KEY);
			continue;
		}

		if (SUCCEED != zbx_json_value_by_name(&jp_row, ZBX_PROTO_TAG_KEY_ORIG, key_orig, sizeof(key_orig)) || '\0' == *key_orig)
			zbx_strlcpy(key_orig, name, sizeof(key_orig));

		if (SUCCEED != zbx_json_value_by_name(&jp_row, ZBX_PROTO_TAG_DELAY, tmp, sizeof(tmp)) || '\0' == *tmp)
		{
			zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",
					ZBX_PROTO_TAG_DELAY);
			continue;
		}

		delay = atoi(tmp);

		if (SUCCEED != zbx_json_value_by_name(&jp_row, ZBX_PROTO_TAG_LOGLASTSIZE, tmp, sizeof(tmp)) || '\0' == *tmp)
		{
			zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",
					ZBX_PROTO_TAG_LOGLASTSIZE);
			continue;
		}

		lastlogsize = atoi(tmp);

		if (SUCCEED != zbx_json_value_by_name(&jp_row, ZBX_PROTO_TAG_MTIME, tmp, sizeof(tmp)) || '\0' == *tmp)
		{
			zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",
					ZBX_PROTO_TAG_MTIME);
			mtime = 0;
		}
		else
		{
			mtime = atoi(tmp);
		}

		add_check(name, key_orig, delay, lastlogsize, mtime);
	}

	clean_regexps_ex(regexps, &regexps_num);

	if (SUCCEED == zbx_json_brackets_by_name(&jp, ZBX_PROTO_TAG_REGEXP, &jp_data))
	{
	 	p = NULL;
		while (NULL != (p = zbx_json_next(&jp_data, p)))
		{
/* {"regexp":[{"name":"regexp1",...,...},{...},...]}
 *            ^------------------------^
 */			if (SUCCEED != zbx_json_brackets_open(p, &jp_row))
				goto json_error;

			if (SUCCEED != zbx_json_value_by_name(&jp_row, "name", name, sizeof(name)))
			{
				zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"", "name");
				continue;
			}

			if (SUCCEED != zbx_json_value_by_name(&jp_row, "expression", expression, sizeof(expression)) || '\0' == *expression)
			{
				zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",	"expression");
				continue;
			}

			if (SUCCEED != zbx_json_value_by_name(&jp_row, "expression_type", tmp, sizeof(tmp)) || *tmp == '\0')
			{
				zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",	"expression_type");
				continue;
			}

			expression_type = atoi(tmp);

			if (SUCCEED != zbx_json_value_by_name(&jp_row, "exp_delimiter", tmp, sizeof(tmp)))
			{
				zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",	"exp_delimiter");
				continue;
			}

			exp_delimiter = tmp[0];

			if (SUCCEED != zbx_json_value_by_name(&jp_row, "case_sensitive", tmp, sizeof(tmp)) || '\0' == *tmp)
			{
				zabbix_log(LOG_LEVEL_WARNING, "Unable to retrieve value of tag \"%s\"",
						"case_sensitive");
				continue;
			}

			case_sensitive = atoi(tmp);

			add_regexp_ex(&regexps, &regexps_alloc, &regexps_num,
					name, expression, expression_type, exp_delimiter, case_sensitive);
		}
	}

	return SUCCEED;
json_error:
	zabbix_log(LOG_LEVEL_ERR, "Can't parse list of active checks. %s",
			zbx_json_strerror());
	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: refresh_active_checks                                            *
 *                                                                            *
 * Purpose: Retrieve from Zabbix server list of active checks                 *
 *                                                                            *
 * Parameters: host - IP or Hostname of Zabbix server                         *
 *             port - port of Zabbix server                                   *
 *                                                                            *
 * Return value: returns SUCCEED on successful parsing,                       *
 *               FAIL on other cases                                          *
 *                                                                            *
 * Author: Eugene Grigorjev, Alexei Vladishev (new json protocol)             *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	refresh_active_checks(const char *host, unsigned short port)
{
	zbx_sock_t	s;
	char		*buf;
	int		ret;
	struct zbx_json	json;

	zabbix_log(LOG_LEVEL_DEBUG, "refresh_active_checks('%s',%u)", host, port);

	zbx_json_init(&json, ZBX_JSON_STAT_BUF_LEN);

	zbx_json_addstring(&json, ZBX_PROTO_TAG_REQUEST, ZBX_PROTO_VALUE_GET_ACTIVE_CHECKS, ZBX_JSON_TYPE_STRING);
	zbx_json_addstring(&json, ZBX_PROTO_TAG_HOST, CONFIG_HOSTNAME, ZBX_JSON_TYPE_STRING);

	if (SUCCEED == (ret = zbx_tcp_connect(&s, CONFIG_SOURCE_IP, host, port, CONFIG_TIMEOUT)))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "Sending [%s]", json.buffer);

		if (SUCCEED == (ret = zbx_tcp_send(&s, json.buffer)))
		{
			zabbix_log(LOG_LEVEL_DEBUG, "Before read");

			if (SUCCEED == (ret = zbx_tcp_recv_ext(&s, &buf, ZBX_TCP_READ_UNTIL_CLOSE, 0)))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "Got [%s]", buf);
				parse_list_of_checks(buf);
			}
		}

		zbx_tcp_close(&s);
	}

	if (SUCCEED != ret)
		zabbix_log(LOG_LEVEL_DEBUG, "Get active checks error: %s", zbx_tcp_strerror());

	zbx_json_free(&json);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: check_response                                                   *
 *                                                                            *
 * Purpose: Check whether JSON response is SUCCEED                            *
 *                                                                            *
 * Parameters: result SUCCEED or FAIL                                         *
 *                                                                            *
 * Return value:  SUCCEED - processed successfully                            *
 *                FAIL - an error occurred                                    *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments: zabbix_sender has almost the same function!                      *
 *                                                                            *
 ******************************************************************************/
static int	check_response(char *response)
{
	const char		*__function_name = "check_response";

	struct zbx_json_parse	jp;
	char			value[MAX_STRING_LEN];
	char			info[MAX_STRING_LEN];
	int			ret = SUCCEED;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() response:'%s'", __function_name, response);

	ret = zbx_json_open(response, &jp);

	if (SUCCEED == ret)
		ret = zbx_json_value_by_name(&jp, ZBX_PROTO_TAG_RESPONSE, value, sizeof(value));

	if (SUCCEED == ret && 0 != strcmp(value, ZBX_PROTO_VALUE_SUCCESS))
		ret = FAIL;

	if (SUCCEED == ret && SUCCEED == zbx_json_value_by_name(&jp, ZBX_PROTO_TAG_INFO, info, sizeof(info)))
		zabbix_log(LOG_LEVEL_DEBUG, "Info from server: %s", info);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: send_buffer                                                      *
 *                                                                            *
 * Purpose: Send value stored in the buffer to Zabbix server                  *
 *                                                                            *
 * Parameters: host - IP or Hostname of Zabbix server                         *
 *             port - port number                                             *
 *                                                                            *
 * Return value: returns SUCCEED on successful parsing,                       *
 *               FAIL on other cases                                          *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	send_buffer(const char *host, unsigned short port)
{
	const char			*__function_name = "send_buffer";
	struct zbx_json 		json;
	ZBX_ACTIVE_BUFFER_ELEMENT	*el;
	zbx_sock_t			s;
	char				*buf = NULL;
	int				ret = SUCCEED, i, now;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() host:'%s' port:%d values:%d/%d",
			__function_name, host, port, buffer.count, CONFIG_BUFFER_SIZE);

	if (0 == buffer.count)
		goto ret;

	now = (int)time(NULL);

	if (CONFIG_BUFFER_SIZE / 2 > buffer.pcount && CONFIG_BUFFER_SIZE > buffer.count &&
			CONFIG_BUFFER_SEND > now - buffer.lastsent)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "Will not send now. Now %d lastsent %d < %d",
				now, buffer.lastsent, CONFIG_BUFFER_SEND);
		goto ret;
	}

	zbx_json_init(&json, ZBX_JSON_STAT_BUF_LEN);
	zbx_json_addstring(&json, ZBX_PROTO_TAG_REQUEST, ZBX_PROTO_VALUE_AGENT_DATA, ZBX_JSON_TYPE_STRING);
	zbx_json_addarray(&json, ZBX_PROTO_TAG_DATA);

	for (i = 0; i < buffer.count; i++)
	{
		el = &buffer.data[i];

		zbx_json_addobject(&json, NULL);
		zbx_json_addstring(&json, ZBX_PROTO_TAG_HOST, el->host, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json, ZBX_PROTO_TAG_KEY, el->key, ZBX_JSON_TYPE_STRING);
		zbx_json_addstring(&json, ZBX_PROTO_TAG_VALUE, el->value, ZBX_JSON_TYPE_STRING);
		if (el->lastlogsize)
			zbx_json_adduint64(&json, ZBX_PROTO_TAG_LOGLASTSIZE, el->lastlogsize);
		if (el->mtime)
			zbx_json_adduint64(&json, ZBX_PROTO_TAG_MTIME, el->mtime);
		if (el->timestamp)
			zbx_json_adduint64(&json, ZBX_PROTO_TAG_LOGTIMESTAMP, el->timestamp);
		if (el->source)
			zbx_json_addstring(&json, ZBX_PROTO_TAG_LOGSOURCE, el->source, ZBX_JSON_TYPE_STRING);
		if (el->severity)
			zbx_json_adduint64(&json, ZBX_PROTO_TAG_LOGSEVERITY, el->severity);
		if (el->logeventid)
			zbx_json_adduint64(&json, ZBX_PROTO_TAG_LOGEVENTID, el->logeventid);
		zbx_json_adduint64(&json, ZBX_PROTO_TAG_CLOCK, el->clock);
		zbx_json_close(&json);
	}

	zbx_json_close(&json);
	zbx_json_adduint64(&json, ZBX_PROTO_TAG_CLOCK, (int)time(NULL));

	if (SUCCEED == (ret = zbx_tcp_connect(&s, CONFIG_SOURCE_IP, host, port,
					MIN(buffer.count * CONFIG_TIMEOUT, 60))))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "JSON before sending [%s]", json.buffer);

		if (SUCCEED == (ret = zbx_tcp_send(&s, json.buffer)))
		{
			if (SUCCEED == zbx_tcp_recv(&s, &buf))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "JSON back [%s]", buf);

				if (NULL == buf || SUCCEED != check_response(buf))
					zabbix_log(LOG_LEVEL_DEBUG, "NOT OK");
				else
					zabbix_log(LOG_LEVEL_DEBUG, "OK");
			}
			else
				zabbix_log(LOG_LEVEL_DEBUG, "Send value error: [recv] %s", zbx_tcp_strerror());
		}
		else
			zabbix_log(LOG_LEVEL_DEBUG, "Send value error: [send] %s", zbx_tcp_strerror());

		zbx_tcp_close(&s);
	}
	else
		zabbix_log(LOG_LEVEL_DEBUG, "Send value error: [connect] %s", zbx_tcp_strerror());

	zbx_json_free(&json);

	if (SUCCEED == ret)
	{
		/* free buffer */
		for (i = 0; i < buffer.count; i++)
		{
			el = &buffer.data[i];

			zbx_free(el->host);
			zbx_free(el->key);
			zbx_free(el->value);
			zbx_free(el->source);
		}
		buffer.count = 0;
		buffer.pcount = 0;
		buffer.lastsent = now;
	}
ret:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: process_value                                                    *
 *                                                                            *
 * Purpose: Buffer new value or send the whole buffer to the server           *
 *                                                                            *
 * Parameters: host - IP or Hostname of Zabbix server                         *
 *             port - port of Zabbix server                                   *
 *             hostname - name of host in Zabbix database                     *
 *             key - name of metric                                           *
 *             value - string version os key value                            *
 *             lastlogsize - size of read logfile                             *
 *             mtime - time of last file modification                         *
 *             timestamp - timestamp of read value                            *
 *             source - name of logged data source                            *
 *             severity - severity of logged data sources                     *
 *             persistent - do not overwrite old values                       *
 *                                                                            *
 * Return value: returns SUCCEED on successful parsing,                       *
 *               FAIL on other cases                                          *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments:                                                                  *
 *                                                                            *
 ******************************************************************************/
static int	process_value(
		const char	*server,
		unsigned short	port,
		const char	*host,
		const char	*key,
		const char	*value,
		long		*lastlogsize,
		int		*mtime,
		unsigned long	*timestamp,
		const char	*source,
		unsigned short	*severity,
		unsigned long	*logeventid,
		unsigned char	persistent
)
{
	const char			*__function_name = "process_value";
	ZBX_ACTIVE_BUFFER_ELEMENT	*el = NULL;
	int				i, ret = FAIL;
	size_t				sz;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() key:'%s:%s' value:'%s'",
			__function_name, host, key, value);

	send_buffer(server, port);

	if (0 != persistent && CONFIG_BUFFER_SIZE / 2 <= buffer.pcount)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Buffer is full, can't store persistent value.");
		goto ret;
	}

	if (CONFIG_BUFFER_SIZE > buffer.count)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "Buffer: new element %d", buffer.count);
		el = &buffer.data[buffer.count];
		buffer.count++;
	}
	else
	{
		if (0 == persistent)
		{
			for (i = 0; i < buffer.count; i++)
			{
				el = &buffer.data[i];
				if (0 == strcmp(el->host, host) && 0 == strcmp(el->key, key))
					break;
			}
		}

		if (0 != persistent || i == buffer.count)
		{
			for (i = 0; i < buffer.count; i++)
			{
				el = &buffer.data[i];
				if (0 == el->persistent)
					break;
			}
		}

		zabbix_log(LOG_LEVEL_DEBUG, "Remove element [%d] Key:'%s:%s'", i, el->host, el->key);

		zbx_free(el->host);
		zbx_free(el->key);
		zbx_free(el->value);
		zbx_free(el->source);

		sz = (CONFIG_BUFFER_SIZE - i - 1) * sizeof(ZBX_ACTIVE_BUFFER_ELEMENT);
		memmove(&buffer.data[i], &buffer.data[i + 1], sz);

		zabbix_log(LOG_LEVEL_DEBUG, "Buffer full: new element %d", buffer.count - 1);

		el = &buffer.data[CONFIG_BUFFER_SIZE - 1];
	}

	memset(el, 0, sizeof(ZBX_ACTIVE_BUFFER_ELEMENT));
	el->host	= strdup(host);
	el->key		= strdup(key);
	el->value	= strdup(value);
	if (source)
		el->source	= strdup(source);
	if (severity)
		el->severity	= *severity;
	if (lastlogsize)
		el->lastlogsize	= *lastlogsize;
	if (mtime) /* will be null for "eventlog" and "log" and the value will be 0, only "logrt" matters */
		el->mtime	= *mtime;
	if (timestamp)
		el->timestamp	= *timestamp;
	if (logeventid)
		el->logeventid	= (int)*logeventid;
	el->clock	= (int)time(NULL);
	el->persistent	= persistent;

	if (0 != persistent)
		buffer.pcount++;

	ret = SUCCEED;
ret:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

static void	process_active_checks(char *server, unsigned short port)
{
	const char	*__function_name = "process_active_checks";
	register int	i, s_count, p_count;
	char		**pvalue;
	int		now, send_err = SUCCEED, ret;
	char		*value = NULL;
	long		lastlogsize;
	int		mtime;
	char		params[MAX_STRING_LEN];
	char		filename[MAX_STRING_LEN];
	char		pattern[MAX_STRING_LEN];
	/* checks `log', `eventlog', `logrt' may contain parameter, which overrides CONFIG_MAX_LINES_PER_SECOND */
	char		maxlines_persec_str[16];
	int		maxlines_persec;
#ifdef _WINDOWS
	unsigned long	timestamp, logeventid;
	unsigned short	severity;
	char		key_severity[MAX_STRING_LEN], str_severity[32] /* for `regex_match_ex' */;
	char		key_source[MAX_STRING_LEN], *source = NULL;
	char		key_logeventid[MAX_STRING_LEN], str_logeventid[8] /* for `regex_match_ex' */;
#endif	/* _WINDOWS */
	char		encoding[32];

	AGENT_RESULT	result;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s('%s',%hu)", __function_name, server, port);

	init_result(&result);

	now = (int)time(NULL);

	for (i = 0; NULL != active_metrics[i].key && SUCCEED == send_err; i++)
	{
		if (active_metrics[i].nextcheck > now)
			continue;

		if (ITEM_STATUS_ACTIVE != active_metrics[i].status)
			continue;

		/* special processing for log files without rotation */
		if (0 == strncmp(active_metrics[i].key, "log[", 4))
		{
			ret = FAIL;

			do { /* simple try realization */
				if (2 != parse_command(active_metrics[i].key, NULL, 0, params, sizeof(params)))
					break;

				if (4 < num_param(params))
					break;

				if (0 != get_param(params, 1, filename, sizeof(filename)))
					break;

				if (0 != get_param(params, 2, pattern, sizeof(pattern)))
					*pattern = '\0';

				if (0 != get_param(params, 3, encoding, sizeof(encoding)))
					*encoding = '\0';

				zbx_strupper(encoding);

				if (0 != get_param(params, 4, maxlines_persec_str, sizeof(maxlines_persec_str)) ||
						'\0' == *maxlines_persec_str)
					maxlines_persec = CONFIG_MAX_LINES_PER_SECOND;
				else if (MIN_VALUE_LINES > (maxlines_persec = atoi(maxlines_persec_str)) ||
						MAX_VALUE_LINES < maxlines_persec)
					break;

				s_count = 0;
				p_count = 0;
				lastlogsize = active_metrics[i].lastlogsize;

				while (SUCCEED == (ret = process_log(filename, &lastlogsize, &value, encoding)))
				{
					if (NULL == value) /* End of file. The file could become empty, must save `lastlogsize'. */
					{
						active_metrics[i].lastlogsize = lastlogsize;
						break;
					}

					if (SUCCEED == regexp_match_ex(regexps, regexps_num, value, pattern, ZBX_CASE_SENSITIVE))
					{
						send_err = process_value(server, port, CONFIG_HOSTNAME,
								active_metrics[i].key_orig, value, &lastlogsize,
								NULL, NULL, NULL, NULL,	NULL, 1);
						s_count++;
					}
					p_count++;

					zbx_free(value);

					if (SUCCEED == send_err)
						active_metrics[i].lastlogsize = lastlogsize;
					else
					{ /* buffer is full, stop processing active checks till the buffer is cleared */
						lastlogsize = active_metrics[i].lastlogsize;
						goto ret;
					}

					/* do not flood Zabbix server if file grows too fast */
					if (s_count >= (maxlines_persec * active_metrics[i].refresh))
						break;

					/* do not flood local system if file grows too fast */
					if (p_count >= (4 * maxlines_persec * active_metrics[i].refresh))
						break;
				} /* while processing a log */

			}
			while (0); /* simple try realization */

			if (FAIL == ret)
			{
				active_metrics[i].status = ITEM_STATUS_NOTSUPPORTED;
				zabbix_log(LOG_LEVEL_WARNING, "Active check [%s] is not supported. Disabled.",
						active_metrics[i].key);

				process_value(server, port, CONFIG_HOSTNAME,
						active_metrics[i].key_orig, "ZBX_NOTSUPPORTED",
						&active_metrics[i].lastlogsize, NULL, NULL, NULL, NULL, NULL, 0);
			}
		}
		/* special processing for log files with rotation */
		else if (0 == strncmp(active_metrics[i].key, "logrt[", 6))
		{
			ret = FAIL;

			do { /* simple try realization */
				if (2 != parse_command(active_metrics[i].key, NULL, 0, params, sizeof(params)))
					break;

				if (4 < num_param(params))
					break;

				if (0 != get_param(params, 1, filename, sizeof(filename)))
					break;

				if (0 != get_param(params, 2, pattern, sizeof(pattern)))
					*pattern = '\0';

				if (0 != get_param(params, 3, encoding, sizeof(encoding)))
					*encoding = '\0';

				zbx_strupper(encoding);

				if (0 != get_param(params, 4, maxlines_persec_str, sizeof(maxlines_persec_str)) ||
						'\0' == *maxlines_persec_str)
					maxlines_persec = CONFIG_MAX_LINES_PER_SECOND;
				else if (MIN_VALUE_LINES > (maxlines_persec = atoi(maxlines_persec_str)) ||
						MAX_VALUE_LINES < maxlines_persec)
					break;

				s_count = 0;
				p_count = 0;
				lastlogsize = active_metrics[i].lastlogsize;
				mtime = active_metrics[i].mtime;

				while (SUCCEED == (ret = process_logrt(filename, &lastlogsize, &mtime, &value, encoding)))
				{
					if (NULL == value) /* End of file. The file could become empty, must save `lastlogsize' and `mtime'. */
					{
						active_metrics[i].lastlogsize = lastlogsize;
						active_metrics[i].mtime	= mtime;
						break;
					}

					if (SUCCEED == regexp_match_ex(regexps, regexps_num, value, pattern, ZBX_CASE_SENSITIVE))
					{
						send_err = process_value(server, port, CONFIG_HOSTNAME,
								active_metrics[i].key_orig, value, &lastlogsize,
								&mtime, NULL, NULL, NULL, NULL, 1);
						s_count++;
					}
					p_count++;

					zbx_free(value);

					if (SUCCEED == send_err)
					{
						active_metrics[i].lastlogsize = lastlogsize;
						active_metrics[i].mtime = mtime;
					}
					else
					{ /* buffer is full, stop processing active checks till the buffer is cleared */
						lastlogsize = active_metrics[i].lastlogsize;
						mtime = active_metrics[i].mtime;
						goto ret;
					}

					/* do not flood Zabbix server if file grows too fast */
					if (s_count >= (maxlines_persec * active_metrics[i].refresh))
						break;

					/* do not flood local system if file grows too fast */
					if (p_count >= (4 * maxlines_persec * active_metrics[i].refresh))
						break;
				} /* while processing a log */

			}
			while (0); /* simple try realization */

			if (FAIL == ret)
			{
				active_metrics[i].status = ITEM_STATUS_NOTSUPPORTED;
				zabbix_log(LOG_LEVEL_WARNING, "Active check [%s] is not supported. Disabled.",
						active_metrics[i].key);

				process_value(server, port, CONFIG_HOSTNAME,
						active_metrics[i].key_orig, "ZBX_NOTSUPPORTED",
						&active_metrics[i].lastlogsize, &active_metrics[i].mtime,
						NULL, NULL, NULL, NULL, 0);
			}
		}
		/* special processing for eventlog */
		else if (0 == strncmp(active_metrics[i].key, "eventlog[", 9))
		{
			ret = FAIL;
#ifdef _WINDOWS
			do{ /* simple try realization */
				if (2 != parse_command(active_metrics[i].key, NULL, 0, params, sizeof(params)))
					break;

				if (6 < num_param(params))
					break;

				if (0 != get_param(params, 1, filename, sizeof(filename)))
					break;

				if (0 != get_param(params, 2, pattern, sizeof(pattern)))
					*pattern = '\0';

				if (0 != get_param(params, 3, key_severity, sizeof(key_severity)))
					*key_severity = '\0';

				if (0 != get_param(params, 4, key_source, sizeof(key_source)))
					*key_source = '\0';

				if (0 != get_param(params, 5, key_logeventid, sizeof(key_logeventid)))
					*key_logeventid = '\0';

				if (0 != get_param(params, 6, maxlines_persec_str, sizeof(maxlines_persec_str)) ||
						'\0' == *maxlines_persec_str)
					maxlines_persec = CONFIG_MAX_LINES_PER_SECOND;
				else if (MIN_VALUE_LINES > (maxlines_persec = atoi(maxlines_persec_str)) ||
						MAX_VALUE_LINES < maxlines_persec)
					break;

				s_count = 0;
				p_count = 0;
				lastlogsize = active_metrics[i].lastlogsize;
				/* "mtime" parameter is not used by "eventlog" checks */

				while (SUCCEED == (ret = process_eventlog(filename, &lastlogsize,
					&timestamp, &source, &severity, &value, &logeventid)))
				{
					if (NULL == value) /* End of file. The eventlog could become empty, must save `lastlogsize'. */
					{
						active_metrics[i].lastlogsize = lastlogsize;
						break;
					}

					switch (severity)
					{
						case EVENTLOG_SUCCESS:
						case EVENTLOG_INFORMATION_TYPE:
							severity = 1;
							zbx_snprintf(str_severity, sizeof(str_severity), INFORMATION_TYPE);
							break;
						case EVENTLOG_WARNING_TYPE:
							severity = 2;
							zbx_snprintf(str_severity, sizeof(str_severity), WARNING_TYPE);
							break;
						case EVENTLOG_ERROR_TYPE:
							severity = 4;
							zbx_snprintf(str_severity, sizeof(str_severity), ERROR_TYPE);
							break;
						case EVENTLOG_AUDIT_FAILURE:
							severity = 7;
							zbx_snprintf(str_severity, sizeof(str_severity), AUDIT_FAILURE);
							break;
						case EVENTLOG_AUDIT_SUCCESS:
							severity = 8;
							zbx_snprintf(str_severity, sizeof(str_severity), AUDIT_SUCCESS);
							break;
					}

					zbx_snprintf(str_logeventid, sizeof(str_logeventid), "%lu", logeventid);

					if (SUCCEED == regexp_match_ex(regexps, regexps_num, value, pattern, ZBX_CASE_SENSITIVE) &&
							SUCCEED == regexp_match_ex(regexps, regexps_num, str_severity,
									key_severity, ZBX_IGNORE_CASE) &&
							(('\0' == *key_source) ? 1 : (0 == strcmp(key_source, source))) &&
							SUCCEED == regexp_match_ex(regexps, regexps_num, str_logeventid,
									key_logeventid, ZBX_CASE_SENSITIVE))
					{
						send_err = process_value(server, port, CONFIG_HOSTNAME,
								active_metrics[i].key_orig, value, &lastlogsize,
								NULL, &timestamp, source, &severity, &logeventid, 1);
						s_count++;
					}
					p_count++;

					zbx_free(source);
					zbx_free(value);

					if (SUCCEED == send_err)
						active_metrics[i].lastlogsize = lastlogsize;
					else
					{ /* buffer is full, stop processing active checks till the buffer is cleared */
						lastlogsize = active_metrics[i].lastlogsize;
						goto ret;
					}

					/* do not flood Zabbix server if file grows too fast */
					if (s_count >= (maxlines_persec * active_metrics[i].refresh))
						break;

					/* do not flood local system if file grows too fast */
					if (p_count >= (4 * maxlines_persec * active_metrics[i].refresh))
						break;
				} /* while processing an eventlog */

				break;

			}
			while (0); /* simple try realization */
#endif	/* _WINDOWS */

			if (FAIL == ret)
			{
				active_metrics[i].status = ITEM_STATUS_NOTSUPPORTED;
				zabbix_log(LOG_LEVEL_WARNING, "Active check [%s] is not supported. Disabled.",
						active_metrics[i].key);

				process_value(server, port, CONFIG_HOSTNAME,
						active_metrics[i].key_orig, "ZBX_NOTSUPPORTED",
						&active_metrics[i].lastlogsize, NULL, NULL, NULL, NULL, NULL, 0);
			}
		}
		else
		{
			process(active_metrics[i].key, 0, &result);

			if (NULL == (pvalue = GET_TEXT_RESULT(&result)))
				pvalue = GET_MSG_RESULT(&result);

			if (NULL != pvalue)
			{
				zabbix_log(LOG_LEVEL_DEBUG, "For key [%s] received value [%s]", active_metrics[i].key, *pvalue);

				process_value(server, port, CONFIG_HOSTNAME,
						active_metrics[i].key_orig, *pvalue, NULL,
						NULL, NULL, NULL, NULL, NULL, 0);

				if (0 == strcmp(*pvalue, "ZBX_NOTSUPPORTED"))
				{
					active_metrics[i].status = ITEM_STATUS_NOTSUPPORTED;
					zabbix_log(LOG_LEVEL_WARNING, "Active check [%s] is not supported. Disabled.",
							active_metrics[i].key);
				}
			}

			free_result(&result);
		}
		active_metrics[i].nextcheck = (int)time(NULL) + active_metrics[i].refresh;
	}
ret:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

ZBX_THREAD_ENTRY(active_checks_thread, args)
{
	ZBX_THREAD_ACTIVECHK_ARGS activechk_args;

	int	nextcheck = 0, nextrefresh = 0, nextsend = 0;
	char	*p = NULL;

	assert(args);
	assert(((zbx_thread_args_t *)args)->args);

	zabbix_log(LOG_LEVEL_WARNING, "agent #%d started [active checks]", ((zbx_thread_args_t *)args)->thread_num);

	activechk_args.host = zbx_strdup(NULL, ((ZBX_THREAD_ACTIVECHK_ARGS *)((zbx_thread_args_t *)args)->args)->host);
	activechk_args.port = ((ZBX_THREAD_ACTIVECHK_ARGS *)((zbx_thread_args_t *)args)->args)->port;

	zbx_free(args);

#ifdef ZABBIX_DAEMON
	set_child_signal_handler();
#endif

	if (NULL != (p = strchr(activechk_args.host, ',')))
		*p = '\0';

	init_active_metrics();

	while (ZBX_IS_RUNNING())
	{
		if (time(NULL) >= nextsend)
		{
			send_buffer(activechk_args.host, activechk_args.port);
			nextsend = (int)time(NULL) + 1;
		}

		if (time(NULL) >= nextrefresh)
		{
			zbx_setproctitle("poller [getting list of active checks]");

			if (FAIL == refresh_active_checks(activechk_args.host, activechk_args.port))
			{
				nextrefresh = (int)time(NULL) + 60;
			}
			else
			{
				nextrefresh = (int)time(NULL) + CONFIG_REFRESH_ACTIVE_CHECKS;
			}
		}

		if (time(NULL) >= nextcheck && CONFIG_BUFFER_SIZE / 2 > buffer.pcount)
		{
			zbx_setproctitle("poller [processing active checks]");

			process_active_checks(activechk_args.host, activechk_args.port);
			if (CONFIG_BUFFER_SIZE / 2 <= buffer.pcount) /* failed to complete processing active checks */
				continue;

			nextcheck = get_min_nextcheck();
			if (FAIL == nextcheck)
				nextcheck = (int)time(NULL) + 60;
		}
		else
		{
			zabbix_log(LOG_LEVEL_DEBUG, "Sleeping for %d second(s)", 1);
			zbx_setproctitle("poller [sleeping for %d second(s)]", 1);
			zbx_sleep(1);
		}
	}

	zbx_free(activechk_args.host);
	free_active_metrics();

	zabbix_log(LOG_LEVEL_INFORMATION, "zabbix_agentd active check stopped");

	ZBX_DO_EXIT();

	zbx_thread_exit(0);
}
