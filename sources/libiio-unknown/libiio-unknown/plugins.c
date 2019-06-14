/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2015 Parrot SA
 * Author: Nicolas Carrier <nicolas.carrier@parrot.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * */
#include <dirent.h>

#include <dlfcn.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fnmatch.h>

#include "iio.h"
#include "iio-private.h"

#include "debug.h"

#define PLUGINS_DEFAULT_DIR "/usr/lib/libiio-plugins"

#ifndef PLUGINS_MAX
#define PLUGINS_MAX 4
#endif

#ifndef PLUGINS_MATCHING_PATTERN
#define PLUGINS_MATCHING_PATTERN "libiio-*.so"
#endif

static int pattern_filter(const struct dirent *d)
{
	return fnmatch(PLUGINS_MATCHING_PATTERN, d->d_name, 0) == 0;
}

static void *plugins[PLUGINS_MAX];
static bool plugins_initialized;

void iio_init_plugins(void)
{
	int ret;
	int n;
	int i;
	struct dirent **namelist;
	char *path;
	void **current_plugin;

	/* don't register plugins twice */
	if (plugins_initialized)
		return;
	plugins_initialized = true;

	const char* plugins_dir = getenv("IIO_PLUGINS_DIR");
	if (!plugins_dir)
		plugins_dir = PLUGINS_DEFAULT_DIR;

	n = scandir(plugins_dir, &namelist, pattern_filter, NULL);
	if (n == -1) {
		DEBUG("%s scandir: %s\n", __func__, strerror(errno));
		return;
	}

	current_plugin = plugins;
	for (i = 0; i < n; i++) {
		ret = asprintf(&path, "%s/%s",
				plugins_dir, namelist[i]->d_name);
		if (ret == -1) {
			ERROR("%s asprintf error\n", __func__);
			return;
		}
		DEBUG("loading plugin %s\n", path);
		*current_plugin = dlopen(path, RTLD_NOW);
		free(path);
		if (!*current_plugin)
			WARNING("%s dlopen: %s\n", __func__, dlerror());
		else
			current_plugin++;
		free(namelist[i]);
	}
	free(namelist);

	iio_context_dump_factories();
}

void iio_cleanup_plugins(void)
{
	int i = PLUGINS_MAX;

	if (!plugins_initialized)
		return;
	plugins_initialized = false;

	while (i--)
		if (plugins[i])
			dlclose(plugins[i]);
}

