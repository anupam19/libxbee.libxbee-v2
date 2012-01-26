/*
  libxbee - a C library to aid the use of Digi's XBee wireless modules
            running in API mode (AP=2).

  Copyright (C) 2009  Attie Grande (attie@attie.co.uk)

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>

#include "internal.h"
#include "plugin.h"
#include "thread.h"
#include "log.h"

#ifdef XBEE_NO_PLUGINS

EXPORT int xbee_pluginLoad(char *filename, struct xbee *xbee, void *arg) {
	return XBEE_ENOTIMPLEMENTED;
}

EXPORT int xbee_pluginUnload(char *filename, struct xbee *xbee) {
	return XBEE_ENOTIMPLEMENTED;
}

int _xbee_pluginUnload(struct plugin_info *plugin, int acceptShutdown) {
	return XBEE_ENOTIMPLEMENTED;
}

struct xbee_mode *xbee_pluginModeGet(char *name) {
	return NULL;
}

#else /* XBEE_NO_PLUGINS */

struct ll_head plugin_list;
int plugins_initialized = 0;

struct plugin_threadInfo {
	struct xbee *xbee;
	struct plugin_info *plugin;
};

void xbee_pluginThread(struct plugin_threadInfo *info) {
	struct plugin_threadInfo i;
	
	memcpy(&i, info, sizeof(struct plugin_threadInfo));
	if (i.plugin->features->threadMode != PLUGIN_THREAD_RESPAWN) {
		xsys_thread_detach_self();
		free(info);
	}
	
	i.plugin->features->thread(i.plugin->xbee, i.plugin->arg, &i.plugin->pluginData);
}

EXPORT int xbee_pluginLoad(char *filename, struct xbee *xbee, void *arg) {
	int ret;
	struct plugin_info *plugin;
	char *realfilename;
	void *p;
	struct plugin_threadInfo *threadInfo;
	
	if (!filename) return XBEE_EMISSINGPARAM;
	if (xbee && !xbee_validate(xbee)) {
		return XBEE_EINVAL;
	}
	
	if (xbee) {
		/* user-facing functions need this form of protection...
			 this means that for the default behavior, the fmap must point at this function! */
		if (!xbee->f->pluginLoad) return XBEE_ENOTIMPLEMENTED;
		if (xbee->f->pluginLoad != xbee_pluginLoad) {
			return xbee->f->pluginLoad(filename, xbee, arg);
		}
	}
	
	if (!plugins_initialized) {
		if (ll_init(&plugin_list)) {
			ret = XBEE_ELINKEDLIST;
			goto die1;
		}
		plugins_initialized = 1;
	}
	
	if ((realfilename = calloc(1, PATH_MAX + 1)) == NULL) {
		ret = XBEE_ENOMEM;
		goto die1;
	}
	if (realpath(filename, realfilename) == NULL) {
		ret = XBEE_EFAILED;
		goto die2;
	}
	/* reallocate the the correct length (and ignore failure) */
	if ((p = realloc(realfilename, sizeof(char) * (strlen(realfilename) + 1))) != NULL) realfilename = p;
	
	for (plugin = NULL; (plugin = ll_get_next(&plugin_list, plugin)) != NULL;) {
		if (plugin->xbee == xbee && !strcmp(realfilename, plugin->filename)) {
			xbee_log(0, "Error while loading plugin - already loaded...");
			ret = XBEE_EINUSE;
			goto die2;
		}
	}
	
	ret = 0;
	
	if ((plugin = calloc(1, sizeof(struct plugin_info))) == NULL) {
		ret = XBEE_ENOMEM;
		goto die2;
	}
	
	plugin->xbee = xbee;
	plugin->arg = arg;
	plugin->filename = realfilename;
	
	if (xbee) {
		xbee_log(5, "Loading plugin on xbee %p... (%s)", xbee, plugin->filename);
	} else {
		xbee_log(5, "Loading plugin... (%s)", plugin->filename);
	}
	
	/* check if the plugin is already resident (plugins can be used by multiple xbee instances */
	if ((plugin->dlHandle = dlopen(plugin->filename, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD)) == NULL) {
		/* it isn't... load the plugin using RTLD_NODELETE so that dlclose()ing won't intefere with other xbee instances */
		if ((plugin->dlHandle = dlopen(plugin->filename, RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE)) == NULL) {
			xbee_log(2, "Error while loading plugin (%s) - %s", plugin->filename, dlerror());
			ret = XBEE_EOPENFAILED;
			goto die3;
		}
	}
	
	if ((plugin->features = dlsym(plugin->dlHandle, "libxbee_features")) == NULL) {
		xbee_log(2, "Error while loading plugin (%s) - Not a valid libxbee plugin", plugin->filename);
		ret = XBEE_EOPENFAILED;
		goto die4;
	}
	
	if (plugin->features->thread) {
		if (plugin->features->threadMode == PLUGIN_THREAD_RESPAWN && !xbee) {
			xbee_log(2, "Cannot load plugin with respawning thread without an xbee instance!");
			ret = XBEE_EINVAL;
			goto die4;
		}
		if ((threadInfo = calloc(1, sizeof(struct plugin_threadInfo))) == NULL) {
			ret = XBEE_ENOMEM;
			goto die4;
		}
		threadInfo->xbee = xbee;
		threadInfo->plugin = plugin;
	}
	
	ll_add_tail(&plugin_list, plugin);
	if (xbee) ll_add_tail(&xbee->pluginList, plugin);
	
	if (plugin->features->init) {
		int ret;
		if ((ret = plugin->features->init(xbee, plugin->arg, &plugin->pluginData)) != 0) {
			xbee_log(2, "Plugin's init() returned non-zero! (%d)...", ret);
			ret = XBEE_EUNKNOWN;
			goto die5;
		}
	}
	
	if (plugin->features->thread) {
		switch (plugin->features->threadMode) {
			case PLUGIN_THREAD_RESPAWN:
				xbee_log(5, "Starting plugin's thread() in RESPAWN mode...");
				if (xbee_threadStartMonitored(xbee, &plugin->thread, xbee_pluginThread, threadInfo)) {
					xbee_log(1, "xbee_threadStartMonitored(plugin->thread)");
					ret = XBEE_ETHREAD;
					goto die5;
				}
				break;
			default:
				xbee_log(2, "Unknown thread mode, running once...\n");
			case PLUGIN_THREAD_RUNONCE:
				if (xsys_thread_create(&plugin->thread, (void *(*)(void*))xbee_pluginThread, threadInfo) != 0) {
					xbee_log(2, "Failed to start plugin's thread()...");
					ret = XBEE_ETHREAD;
					goto die5;
				}
		}
	}
	
	goto done;
die5:
	if (xbee) ll_ext_item(&xbee->pluginList, plugin);
	ll_ext_item(&plugin_list, plugin);
die4:
	dlclose(plugin->dlHandle);
die3:
	free(plugin);
die2:
	free(realfilename);
die1:
done:
	return ret;
}

int _xbee_pluginUnload(struct plugin_info *plugin, int acceptShutdown) {
	struct xbee *xbee;
	int ret;
	
	ret = 0;
	xbee = plugin->xbee;
	
	if (plugin->features->thread) {
		if (plugin->features->threadMode == PLUGIN_THREAD_RESPAWN) {
			if (plugin->xbee && !_xbee_validate(plugin->xbee, acceptShutdown)) {
				xbee_log(-1, "Cannot remove plugin with respawning thread... xbee instance missing! %p", xbee);
				ret = XBEE_EINVAL;
				goto die1;
			}
			xbee_threadStopMonitored(plugin->xbee, &plugin->thread, NULL, NULL);
		} else {
			xsys_thread_cancel(&plugin->thread);
		}
	}

	if (plugin->xbee) ll_ext_item(&plugin->xbee->pluginList, plugin);
	
	ll_ext_item(&plugin_list, plugin);
	
	if (plugin->features->remove) plugin->features->remove(plugin->xbee, plugin->arg, &plugin->pluginData);
	
	dlclose(plugin->dlHandle);
	
	free(plugin);
	
die1:
	return ret;
}
EXPORT int xbee_pluginUnload(char *filename, struct xbee *xbee) {
	int ret;
	char *realfilename;
	struct plugin_info *plugin;
	void *p;
	
	if (!filename) return XBEE_EMISSINGPARAM;
	if (xbee && !xbee_validate(xbee)) {
		return XBEE_EINVAL;
	}
	
	if (xbee) {
		/* user-facing functions need this form of protection...
			 this means that for the default behavior, the fmap must point at this function! */
		if (!xbee->f->pluginUnload) return XBEE_ENOTIMPLEMENTED;
		if (xbee->f->pluginUnload != xbee_pluginUnload) {
			return xbee->f->pluginUnload(filename, xbee);
		}
	}
	
	ret = 0;
	
	if ((realfilename = calloc(1, PATH_MAX + 1)) == NULL) {
		ret = XBEE_ENOMEM;
		goto die1;
	}
	if (realpath(filename, realfilename) == NULL) {
		ret = XBEE_EFAILED;
		goto die2;
	}
	/* reallocate the the correct length (and ignore failure) */
	if ((p = realloc(realfilename, sizeof(char) * (strlen(realfilename) + 1))) != NULL) realfilename = p;
	
	for (plugin = NULL; (plugin = ll_get_next(&plugin_list, plugin)) != NULL;) {
		if (plugin->xbee == xbee && !strcmp(realfilename, plugin->filename)) break;
	}
	
	if (plugin) ret = _xbee_pluginUnload(plugin, 0);
	
die2:
	free(realfilename);
die1:
	return ret;
}

struct xbee_mode *xbee_pluginModeGet(char *name) {
	int i;
	struct plugin_info *plugin;
	struct xbee_mode **xbee_modes;
	if (!name) return NULL;
	if (!plugins_initialized) return NULL;
	
	for (plugin = NULL; (plugin = ll_get_next(&plugin_list, plugin)) != NULL;) {
		if (!plugin->features->xbee_modes) continue;
		xbee_modes = plugin->features->xbee_modes;
		for (i = 0; xbee_modes[i]; i++) {
			if (!strcasecmp(xbee_modes[i]->name, name)) return xbee_modes[i];
		}
	}
	
	return NULL;
}

#endif /* XBEE_NO_PLUGINS */
