/* -------------------------------------------------------------------------
 *
 * block_access.c
 *
 * Copyright (c) 2017-2018, Euler Taveira de Oliveira
 *
 * IDENTIFICATION
 *		block_access/block_access.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libpq/auth.h"
#include "port.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

typedef struct BATime {
	int			hour;			/* 0 .. 23 */
	int			minute;			/* 0 .. 59 */
} BATime;

/*
 * This data structure defines an interval (start_time until end_time) per week
 * day(s) that access will be allowed. It also specifies a set of roles that
 * are excluded from access block if current date/time is out of the specified
 * interval.
 */
typedef struct BAIntervalRole {
	int		*wday;		/* sun (0), mon (1), tue (2), wed (3), thu (4), fri (5), sat (6) */
	int		nwday;
	BATime	start_time;
	BATime	end_time;

	int		nroles;
	char	**roles;
} BAIntervalRole;

static char *trim(char *s);
static char *strtok_all(char * s, char const *d);
static void parse_interval(BAIntervalRole *i, char *s);
static void parse_roles(BAIntervalRole *i, char *s);
static void parse_options(BAIntervalRole *i, int n);

void		_PG_init(void);

/* GUC Variables */
static char		*interval_time = NULL;
static char		*exclude_roles = NULL;

/* Original Hook */
static ClientAuthentication_hook_type original_client_auth_hook = NULL;

/*
 * Strip whitespace from the beginning and end of the string
 *
 * space (0x20), form feed (0x0c), line feed (0x0a), carriage return (0x0d),
 * horizontal tab (0x09) and vertical tab (0x0b) are removed. If s is NULL,
 * return NULL. If s contains only whitespaces, return NULL.
 */
static char *
trim(char *s)
{
	char	*start;
	char	*end;
	char	*t;
	size_t	len;

	if (s == NULL)
		return NULL;

	len = strlen(s);
	start = s;
	end = start + len - 1;

	while (isspace(*start))
		start++;

	while (isspace(*end))
		end--;

	if (end - start >= 0)
	{
		t = palloc0((end - start + 2) * sizeof(char));
		strncpy(t, start, end - start + 1);
	}
	else
	{
		t = NULL;
	}

	return t;
}

/*
 * Same as strtok() except that it returns all tokens even if the token is
 * empty.
 *
 * FIXME if all items are empty, it does not return the last item
 */
static char *
strtok_all(char *str, char const *delims)
{
	static char	*src = NULL;
	char		*ret = 0;
	char		*p;

	if (str != NULL)
		src = str;

	if (src == NULL)
		return NULL;

	if ((p = strpbrk(src, delims)) != NULL)
	{
		*p  = 0;
		ret = src;
		src = ++p;
	}
	else if (*src)
	{
		ret = src;
		src = NULL;
	}

	return ret;
}

/*
 * Each interval item contains:
 * (i) list of abbrev week days separated by comma;
 * (ii) dash;
 * (iii) start time;
 * (iv) dash;
 * (v) end time;
 *
 * Example: mon, wed, fri, sat - 08:00-12:00
 *
 */
static void
parse_interval(BAIntervalRole *interval, char *s)
{
	char	*item;
	char	*item_wd;
	char	*ptr;
	char	*weekday_str;
	char	*start_time_str;
	char	*end_time_str;
	int		i;

	/* debug purposes */
	char	week_day_names[7][4] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

	item = pstrdup(s);

	ptr = strtok(item, "-");
	if (ptr == NULL)
		elog(ERROR, "parse week day failed: %s", s);

	weekday_str = trim(ptr);

	ptr = strtok(NULL, "-");
	if (ptr == NULL)
		elog(ERROR, "parse start time failed: %s", s);

	start_time_str = trim(ptr);

	ptr = strtok(NULL, "-");
	if (ptr == NULL)
		elog(ERROR, "parse end time failed: %s", s);

	end_time_str = trim(ptr);

	elog(DEBUG1, "week days: \"%s\" ; start time: \"%s\" ; end time: \"%s\"", weekday_str, start_time_str, end_time_str);

	pfree(item);

	item = pstrdup(weekday_str);

	/* number of week days */
	interval->nwday = 1;		/* we should have at least one token */
	for (ptr = item; *ptr != '\0'; ptr++)
	{
		if (*ptr == ',')
			interval->nwday++;
	}

	interval->wday = (int *) palloc(interval->nwday * sizeof(int));

	/* week days such as 'mon,wed,fri,sat' */
	i = 0;
	ptr = strtok(item, ",");
	while (ptr)
	{
		item_wd = trim(ptr);
		if (strcmp(item_wd, "sun") == 0)
			interval->wday[i++] = 0;	/* sunday */
		else if (strcmp(item_wd, "mon") == 0)
			interval->wday[i++] = 1;	/* monday */
		else if (strcmp(item_wd, "tue") == 0)
			interval->wday[i++] = 2;	/* tuesday */
		else if (strcmp(item_wd, "wed") == 0)
			interval->wday[i++] = 3;	/* wednesday */
		else if (strcmp(item_wd, "thu") == 0)
			interval->wday[i++] = 4;	/* thursday */
		else if (strcmp(item_wd, "fri") == 0)
			interval->wday[i++] = 5;	/* friday */
		else if (strcmp(item_wd, "sat") == 0)
			interval->wday[i++] = 6;	/* saturday */
		else
			elog(ERROR, "parse week days failed: \"%s\" -> %s", item_wd, weekday_str);

		elog(DEBUG2, "week day: \"%s\"", week_day_names[interval->wday[i - 1]]);

		pfree(item_wd);

		ptr = strtok(NULL, ",");
	}

	pfree(item);

	item = pstrdup(start_time_str);

	/* start time such as '08:00' */
	ptr = strtok(item, ":");
	if (ptr == NULL)
		elog(ERROR, "parse start hour failed: %s", start_time_str);

	interval->start_time.hour = atoi(ptr);
	if (interval->start_time.hour < 0 || interval->start_time.hour > 23)
		elog(ERROR, "parse start hour failed: out of range (%d)", interval->start_time.hour);

	ptr = strtok(NULL, ":");
	if (ptr == NULL)
		elog(ERROR, "parse start minute failed: %s", start_time_str);

	interval->start_time.minute = atoi(ptr);
	if (interval->start_time.minute < 0 || interval->start_time.minute > 59)
		elog(ERROR, "parse start minute failed: out of range (%d)", interval->start_time.minute);
	
	elog(DEBUG2, "start time: hour: %d minute: %d", interval->start_time.hour, interval->start_time.minute);

	pfree(item);

	item = pstrdup(end_time_str);

	/* end time such as '18:00' */
	ptr = strtok(item, ":");
	if (ptr == NULL)
		elog(ERROR, "parse end hour failed: %s", end_time_str);

	interval->end_time.hour = atoi(ptr);
	if (interval->end_time.hour < 0 || interval->end_time.hour > 23)
		elog(ERROR, "parse end hour failed: out of range (%d)", interval->end_time.hour);

	ptr = strtok(NULL, ":");
	if (ptr == NULL)
		elog(ERROR, "parse end minute failed: %s", end_time_str);

	interval->end_time.minute = atoi(ptr);
	if (interval->end_time.minute < 0 || interval->end_time.minute > 59)
		elog(ERROR, "parse end minute failed: out of range (%d)", interval->end_time.minute);

	elog(DEBUG2, "end time: hour: %d minute: %d", interval->end_time.hour, interval->end_time.minute);

	pfree(item);
	pfree(weekday_str);
	pfree(start_time_str);
	pfree(end_time_str);
}

/*
 * Each item of exclude_roles list are separated by comma.
 *
 * Example: foo, bar, baz, euler, jose
 *
 */
static void
parse_roles(BAIntervalRole *interval, char *s)
{
	char	*item;
	char	*ptr;
	int		i;

	if (s == NULL)
	{
		elog(DEBUG1, "role group is empty");
		interval->nroles = 0;
		interval->roles = NULL;

		return;
	}

	item = pstrdup(s);

	/* number of roles */
	interval->nroles = 1;		/* we should have at least one role */
	for (ptr = item; *ptr != '\0'; ptr++)
	{
		if (*ptr == ',')
			interval->nroles++;
	}

	elog(DEBUG1, "role group \"%s\"", item);

	interval->roles = (char **) palloc(interval->nroles * sizeof(char *));

	/* store each role */
	i = 0;
	ptr = strtok(item, ",");
	while (ptr)
	{
		interval->roles[i++] = trim(ptr);
		elog(DEBUG2, "role: \"%s\"", interval->roles[i - 1]);
		ptr = strtok(NULL, ",");
	}

	pfree(item);
}

/*
 * interval_time
 * mon,tue,wed,thu,fri - 08:00-18:00; sat - 08:00-12:00
 *
 * exclude_roles
 * foo,bar,baz ; euler, jose
 */
static void
parse_options(BAIntervalRole *ir, int n)
{
	char	*intervals_str;
	char	*roles_str;
	char	*ptr;
	char	**item;
	int		i;

	/* no intervals, no access block */
	if (interval_time == NULL)
		return;

	/*
	 * interval_time and exclude_roles shouldn't be modified, hence store
	 * content in new variables.
	 */
	intervals_str = trim(interval_time);
	roles_str = trim(exclude_roles);

	/* store each token them parse'em */
	/* FIXME palloc0 because of strtok_all */
	item = (char **) palloc0(n * sizeof(char *));
	i = 0;
	ptr = strtok(intervals_str, ";");
	while (ptr)
	{
		item[i++] = trim(ptr);
		ptr = strtok(NULL, ";");
	}

	/* process each interval item */
	for (i = 0; i < n; i++)
	{
		parse_interval(&ir[i], item[i]);
		pfree(item[i]);
	}
	pfree(item);

	/* store each token them parse'em */
	/* FIXME palloc0 because of strtok_all */
	item = (char **) palloc0(n * sizeof(char *));
	i = 0;
	ptr = strtok_all(roles_str, ";");
	while (ptr)
	{
		item[i++] = trim(ptr);
		elog(DEBUG1, "XXX: %s", item[i - 1]);
		ptr = strtok_all(NULL, ";");
	}

	/* process each roles item */
	for (i = 0; i < n; i++)
	{
		parse_roles(&ir[i], item[i]);
		if (item[i])
			pfree(item[i]);
	}
	pfree(item);

	pfree(intervals_str);
	pfree(roles_str);
}

/*
 * Check authentication
 */
static void
block_access_checks(Port *port, int status)
{
	/* assume that we initially do not have intervals */
	BAIntervalRole	*intervals = NULL;
	int				nintervals = 0;
	int				nroles = 0;

	/*
	 * Any other plugins which use ClientAuthentication_hook.
	 */
	if (original_client_auth_hook)
		original_client_auth_hook(port, status);

	if (interval_time != NULL)
		elog(DEBUG1, "interval_time: %s", interval_time);

	if (exclude_roles != NULL)
		elog(DEBUG1, "exclude_roles: %s", exclude_roles);

	/* apply block access per interval time / role */
	if (status == STATUS_OK && interval_time != NULL)
	{
		bool		bailout = false;

		time_t		t;
		struct tm	*now;
		int			i, j, k;
		char		*ptr;
		char		week_day_names[7][4] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

#ifndef WIN32
		struct timespec	before;
		struct timespec after;
		double posix_wall;

		clock_gettime(CLOCK_MONOTONIC, &before);
#endif

		/* number of intervals */
		nintervals = 1;		/* we should have at least one token */
		for (ptr = interval_time; *ptr != '\0'; ptr++)
			if (*ptr == ';')
				nintervals++;

		elog(DEBUG2, "number of intervals: %d", nintervals);

		/* number of roles */
		nroles = 1;		/* we should have at least one token */
		for (ptr = exclude_roles; *ptr != '\0'; ptr++)
			if (*ptr == ';')
				nroles++;

		elog(DEBUG2, "number of role groups: %d", nroles);

		/* set of intervals x set of roles mismatch */
		if (nintervals != nroles)
			elog(ERROR, "number of intervals and exclude_roles elements do not match");

		intervals = (BAIntervalRole *) palloc0(nintervals * sizeof(BAIntervalRole));

		/* parse block_access.intervals and fills variable 'intervals' */
		parse_options(intervals, nintervals);

		/* actual date and time */
		t = time(NULL);
		now = localtime(&t);

		/* search current date/time in the specified intervals */
		for (i = 0; i < nintervals; i++)
		{
			/* we already find a week day but don't block access */
			if (bailout)
				break;

			for (j = 0; j < intervals[i].nwday; j++)
			{

				elog(DEBUG1, "interval: \"%s\" %02d:%02d - %02d:%02d ; now: \"%s\" %02d:%02d",
								week_day_names[intervals[i].wday[j]],
								intervals[i].start_time.hour, intervals[i].start_time.minute,
								intervals[i].end_time.hour, intervals[i].end_time.minute,
								week_day_names[now->tm_wday],
								now->tm_hour, now->tm_min);

				/* same week day */
				if (intervals[i].wday[j] == now->tm_wday)
				{
					int s1, s2, n1;

					elog(DEBUG2, "found week day: \"%s\"", week_day_names[now->tm_wday]);

					/* time in minutes */
					s1 = intervals[i].start_time.hour * 60 + intervals[i].start_time.minute;
					s2 = intervals[i].end_time.hour * 60 + intervals[i].end_time.minute;
					n1 = now->tm_hour * 60 + now->tm_min;

					/* now is outside interval time */
					if (n1 < s1 || n1 > s2)
					{
						int	found = false;

						elog(DEBUG1, "outside interval time");

						/* role is not found, then bail out */
						for (k = 0; k < intervals[i].nroles; k++)
						{
							if (strcmp(intervals[i].roles[k], port->user_name) == 0)
							{
								elog(DEBUG1, "role \"%s\" in exclude_roles", port->user_name);

								found = true;
								break;
							}
						}

						if (!found)
							elog(ERROR, "access denied because it is outside permitted date and time");
					}

					/* we are not expecting to find more than one week day in different interval times */
					bailout = true;
					break;
				}
			}

			for (j = 0; j < intervals[i].nroles; j++)
				if (intervals[i].roles[j])
					pfree(intervals[i].roles[j]);
		}

		pfree(intervals);

#ifndef WIN32
		clock_gettime(CLOCK_MONOTONIC, &after);

		posix_wall = (1000.0 * after.tv_sec + 1e-6 * after.tv_nsec) -
						(1000.0 * before.tv_sec + 1e-6 * before.tv_nsec);

		elog(DEBUG1, "diff: %.4f ms", posix_wall);
#endif

		elog(INFO, "access allowed");
	}
}

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
	/*
	 * mon, tue, wed, thu, fri - 08:00-18:00 ; sat - 08:00-12:00
	 *
	 * Holds a set of intervals such as the above string. This string will be
	 * parsed and stored in the BAIntervalRole struct. Intervals are separated
	 * by semicolon (;).
	 */
	DefineCustomStringVariable("block_access.intervals",
							"Allow users only between the intervals",
							NULL,
							&interval_time,
							NULL,
							PGC_SIGHUP, 0,
							NULL, NULL, NULL);

	/*
	 * foo,bar,baz ; euler, jose
	 *
	 * Holds a set of roles such as the above string. This string will be
	 * parsed and stored in the BAIntervalRole struct. Group of roles are
	 * separated by semicolon (;). There should be exact one group of roles per
	 * interval. Unfortunately, you cannot specify a role group (role that
	 * contains other roles) because this module does not have access to
	 * catalog, hence, it is impossible to know who are the role members.
	 */
	DefineCustomStringVariable("block_access.exclude_roles",
							"Allow users after the intervals",
							NULL,
							&exclude_roles,
							NULL,
							PGC_SIGHUP, 0,
							NULL, NULL, NULL);

	/* Install Hooks */
	original_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = block_access_checks;
}
