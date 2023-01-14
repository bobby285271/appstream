/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2022-2023 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:as-system-info
 * @short_description: Read information about the current OS and device.
 * @include: appstream.h
 *
 * This class reads information about the current operating system and device
 * that AppStream is running on. It can also be used by GUI toolkits to set
 * data that we can not automatically determine in a toolkit-independent way,
 * such as screen dimensions.
 *
 * AppStream uses this information to verify component relations
 * (as set in requires/recommends/supports etc. tags).
 *
 * See also: #AsComponent
 */

#include "config.h"
#include "as-system-info-private.h"

#include <gio/gio.h>
#include <errno.h>
#include <sys/utsname.h>
#include <dirent.h>

#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#ifdef HAVE_UDEV
#include <libudev.h>
#endif

#include "as-utils-private.h"

#define MB_IN_BYTES (1024 * 1024)

typedef struct
{
	gchar		*os_id;
	gchar		*os_cid;
	gchar		*os_name;
	gchar		*os_version;
	gchar		*os_homepage;

	gchar		*kernel_name;
	gchar		*kernel_version;

	gulong		memory_total;

	GPtrArray	*modaliases;
	GHashTable	*modalias_to_sysfs;
#ifdef HAVE_UDEV
	struct udev	*udev;
#endif
} AsSystemInfoPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (AsSystemInfo, as_system_info, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (as_system_info_get_instance_private (o))

static void
as_system_info_init (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);

	priv->modaliases = g_ptr_array_new ();
	priv->modalias_to_sysfs = g_hash_table_new_full (g_str_hash, g_str_equal,
							 g_free, g_free);
}

static void
as_system_info_finalize (GObject *object)
{
	AsSystemInfo *sysinfo = AS_SYSTEM_INFO (object);
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);

	g_free (priv->os_id);
	g_free (priv->os_cid);
	g_free (priv->os_name);
	g_free (priv->os_version);
	g_free (priv->os_homepage);

	g_free (priv->kernel_name);
	g_free (priv->kernel_version);

	g_ptr_array_unref (priv->modaliases);
	g_hash_table_unref (priv->modalias_to_sysfs);
#ifdef HAVE_UDEV
	udev_unref (priv->udev);
#endif

	G_OBJECT_CLASS (as_system_info_parent_class)->finalize (object);
}

static void
as_system_info_class_init (AsSystemInfoClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = as_system_info_finalize;
}

/**
 * as_system_info_load_os_release:
 */
void
as_system_info_load_os_release (AsSystemInfo *sysinfo, const gchar *os_release_fname)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	g_autoptr(GFile) f = NULL;
	g_autoptr(GError) error = NULL;
	gchar *line;

	/* skip if we already loaded data */
	if (priv->os_id != NULL)
		return;

	if (os_release_fname == NULL) {
		if (g_file_test ("/etc/os-release", G_FILE_TEST_EXISTS))
			os_release_fname = "/etc/os-release";
		else
			os_release_fname = "/usr/lib/os-release";
	}

	/* get details about the distribution we are running on */
	f = g_file_new_for_path (os_release_fname);
	if (g_file_query_exists (f, NULL)) {
		g_autoptr(GDataInputStream) dis = NULL;
		g_autoptr(GFileInputStream) fis = NULL;

		fis = g_file_read (f, NULL, &error);
		if (error != NULL) {
			g_warning ("Unable to read file '%s': %s",
				   os_release_fname, error->message);
			return;
		}
		dis = g_data_input_stream_new ((GInputStream*) fis);

		while ((line = g_data_input_stream_read_line (dis, NULL, NULL, &error)) != NULL) {
			g_auto(GStrv) data = NULL;
			g_autofree gchar *dvalue = NULL;
			if (error != NULL) {
				g_warning ("Unable to read line in file '%s': %s",
					   os_release_fname, error->message);
				g_free (line);
				return;
			}

			data = g_strsplit (line, "=", 2);
			if (g_strv_length (data) != 2) {
				g_free (line);
				continue;
			}

			dvalue = g_strdup (data[1]);
			if (g_str_has_prefix (dvalue, "\"")) {
				gchar *tmp;
				tmp = g_strndup (dvalue + 1, strlen(dvalue) - 2);
				g_free (dvalue);
				dvalue = tmp;
			}

			if (g_strcmp0 (data[0], "ID") == 0) {
				g_free (priv->os_id);
				priv->os_id = g_steal_pointer (&dvalue);

			} else if (g_strcmp0 (data[0], "NAME") == 0) {
				g_free (priv->os_name);
				priv->os_name = g_steal_pointer (&dvalue);

			} else if (g_strcmp0 (data[0], "VERSION_ID") == 0) {
				g_free (priv->os_version);
				priv->os_version = g_steal_pointer (&dvalue);

			} else if (g_strcmp0 (data[0], "HOME_URL") == 0) {
				g_free (priv->os_homepage);
				priv->os_homepage = g_steal_pointer (&dvalue);
			}

			g_free (line);
		}
	}
}

/**
 * as_system_info_get_os_id:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the ID of the current operating system.
 *
 * Returns: the current OS ID.
 */
const gchar*
as_system_info_get_os_id (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_load_os_release (sysinfo, NULL);
	return priv->os_id;
}

/**
 * as_system_info_get_os_cid:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the AppStream component ID of the current operating system.
 *
 * Returns: the component ID of the current OS.
 */
const gchar*
as_system_info_get_os_cid (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);

	as_system_info_load_os_release (sysinfo, NULL);
	if (priv->os_cid != NULL)
		return priv->os_cid;
	if (priv->os_homepage == NULL) {
		priv->os_cid = g_strdup (priv->os_id);
		return priv->os_cid;
	}

	priv->os_cid = as_utils_dns_to_rdns (priv->os_homepage, priv->os_id);
	if (priv->os_cid == NULL)
		return priv->os_id;
	return priv->os_cid;
}

/**
 * as_system_info_get_os_name:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the humen-readable name of the current operating system.
 *
 * Returns: the name of the current OS.
 */
const gchar*
as_system_info_get_os_name (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_load_os_release (sysinfo, NULL);
	return priv->os_name;
}

/**
 * as_system_info_get_os_version:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the version string of the current operating system.
 *
 * Returns: the version of the current OS.
 */
const gchar*
as_system_info_get_os_version (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_load_os_release (sysinfo, NULL);
	return priv->os_version;
}

/**
 * as_system_info_get_os_homepage:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the homepage URL of the current operating system.
 *
 * Returns: the homepage of the current OS.
 */
const gchar*
as_system_info_get_os_homepage (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_load_os_release (sysinfo, NULL);
	return priv->os_homepage;
}

/**
 * as_system_info_read_kernel_details:
 */
static void
as_system_info_read_kernel_details (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	struct utsname utsbuf;
	gchar *tmp;

	if (priv->kernel_name != NULL)
		return;

	if (uname (&utsbuf) != 0) {
		g_warning ("Unable to read kernel information via uname: %s",
			   g_strerror (errno));
		return;
	}

	g_free (priv->kernel_name);
	priv->kernel_name = g_strdup (utsbuf.sysname);
	tmp = g_strrstr (utsbuf.release, "-");
	if (tmp != NULL)
		tmp[0] = '\0';

	g_free (priv->kernel_version);
	priv->kernel_version = g_strdup (utsbuf.release);
}

/**
 * as_system_info_get_kernel_name:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the name of the current kernel, e.g. "Linux"
 *
 * Returns: the current OS kernel name
 */
const gchar*
as_system_info_get_kernel_name (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_read_kernel_details (sysinfo);
	return priv->kernel_name;
}

/**
 * as_system_info_get_kernel_version:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the version of the current kernel, e.g. "6.2.0-2"
 *
 * Returns: the current kernel version
 */
const gchar*
as_system_info_get_kernel_version (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_read_kernel_details (sysinfo);
	return priv->kernel_version;
}

/**
 * as_get_physical_memory_total:
 */
static gulong
as_get_physical_memory_total (void)
{
#if defined(__linux__)
	struct sysinfo si = { 0 };
	sysinfo (&si);
	if (si.mem_unit > 0)
		return (si.totalram * si.mem_unit) / MB_IN_BYTES;
	return 0;
#elif defined(__FreeBSD__)
	unsigned long physmem;
	sysctl ((int[]){ CTL_HW, HW_PHYSMEM }, 2, &physmem, &(size_t){ sizeof (physmem) }, NULL, 0);
	return physmem / MB_IN_BYTES;
#else
#error "Implementation of as_get_physical_memory_total() missing for this OS."
#endif
}

/**
 * as_system_info_get_memory_total:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get the current total amount of physical memory in MiB.
 *
 * Returns: the current total amount of usable memory in MiB
 */
gulong
as_system_info_get_memory_total (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	if (priv->memory_total == 0)
		priv->memory_total = as_get_physical_memory_total ();
	return priv->memory_total;
}

/**
 * as_system_info_populate_modaliases_map_cb:
 */
static gboolean
as_system_info_populate_modaliases_map_cb (AsSystemInfo *sysinfo, const gchar *root_path)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	DIR *dir;
	struct dirent *ent;

	if ((dir = opendir (root_path)) != NULL) {
		while ((ent = readdir (dir)) != NULL) {
			if (ent->d_type == DT_LNK)
				continue;

			if (ent->d_type == DT_DIR) {
				g_autofree gchar *subdir_path = g_build_filename (root_path, ent->d_name, NULL);
				if (!as_str_equal0 (ent->d_name, ".") && !as_str_equal0 (ent->d_name, ".."))
					as_system_info_populate_modaliases_map_cb (sysinfo, subdir_path);
			} else {
				if (as_str_equal0 (ent->d_name, "modalias")) {
					gchar *contents = NULL;
					g_autoptr(GError) read_error = NULL;
					g_autofree gchar *path = g_build_filename (root_path, ent->d_name, NULL);

					if (!g_file_get_contents (path, &contents, NULL, &read_error)) {
						g_warning ("Error while reading modalias file %s: %s", path, read_error->message);
						closedir (dir);
						return FALSE;
					}
					contents = as_strstripnl (contents);
					g_hash_table_insert (priv->modalias_to_sysfs,
								contents,
								g_path_get_dirname (path));
				}
			}
		}
		closedir (dir);
	} else {
		g_warning ("Error while searching for modalias entries in %s: %s", root_path, g_strerror (errno));
	}

	return TRUE;
}

/**
 * as_system_info_populate_modaliases:
 */
static void
as_system_info_populate_modaliases (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	GHashTableIter ht_iter;
	gpointer ht_key;

	/* we never want to run this multiple times */
	if (priv->modaliases->len != 0)
		return;

	as_system_info_populate_modaliases_map_cb (sysinfo, "/sys/devices");
	g_hash_table_iter_init (&ht_iter, priv->modalias_to_sysfs);
	while (g_hash_table_iter_next (&ht_iter, &ht_key, NULL))
		g_ptr_array_add (priv->modaliases, ht_key);
}

/**
 * as_system_info_get_modaliases:
 * @sysinfo: a #AsSystemInfo instance.
 *
 * Get a list of modaliases for all the hardware on this system that has them.
 *
 * Returns: (transfer none) (element-type utf8): a list of modaliases on the system.
 */
GPtrArray*
as_system_info_get_modaliases (AsSystemInfo *sysinfo)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_populate_modaliases (sysinfo);
	return priv->modaliases;
}

/**
 * as_system_info_modalias_to_syspath:
 * @sysinfo: a #AsSystemInfo instance.
 * @modalias: the modalias value to resolve.
 *
 * Receive a path in /sys for the devices with the given modalias.
 *
 * Returns: the syspath, or %NULL if none was found.
 */
const gchar*
as_system_info_modalias_to_syspath (AsSystemInfo *sysinfo, const gchar *modalias)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
	as_system_info_populate_modaliases (sysinfo);
	return g_hash_table_lookup (priv->modalias_to_sysfs, modalias);
}

/**
 * as_system_info_get_device_name_for_modalias:
 * @sysinfo: a #AsSystemInfo instance.
 * @modalias: the modalias value to resolve.
 * @error: a #GError
 *
 * Return a human readable device name for the given modalias.
 * Will return the modalias again if no device name could be found,
 * and returns %NULL on error.
 *
 * Returns: a human-readable device name, or %NULL on error.
 */
gchar*
as_system_info_get_device_name_for_modalias (AsSystemInfo *sysinfo, const gchar *modalias, GError **error)
{
	AsSystemInfoPrivate *priv = GET_PRIVATE (sysinfo);
#ifdef HAVE_UDEV
	struct udev_device *device;
	struct udev_list_entry *entry, *e;
	const gchar *device_vendor = NULL;
	const gchar *device_model = NULL;
	const gchar *usb_class = NULL;
	const gchar *driver = NULL;
	const gchar *syspath = NULL;
	gchar *result = NULL;

	syspath = g_hash_table_lookup (priv->modalias_to_sysfs, modalias);
	if (syspath == NULL) {
		g_set_error (error,
				AS_UTILS_ERROR,
				AS_UTILS_ERROR_FAILED,
				"No path found for device with modalias '%s'.", modalias);
		return NULL;
	}

	if (priv->udev == NULL)
		priv->udev = udev_new ();

	device = udev_device_new_from_syspath (priv->udev, syspath);
	if (device == NULL) {
		g_set_error (error,
				AS_UTILS_ERROR,
				AS_UTILS_ERROR_FAILED,
				"Failure to read device information for %s: %s",
				modalias, g_strerror (errno));
		return NULL;
	}

	entry = udev_device_get_properties_list_entry (device);
        udev_list_entry_foreach(e, entry) {
		const gchar *e_name = udev_list_entry_get_name (e);
		if (g_strstr_len (e_name, -1, "_VENDOR") != NULL)
			device_vendor = udev_list_entry_get_value (e);
		else if (g_strstr_len (e_name, -1, "_MODEL") != NULL)
			device_model = udev_list_entry_get_value (e);
		else if (as_str_equal0 (e_name, "DRIVER"))
			driver = udev_list_entry_get_value (e);
		else if (g_strstr_len (e_name, -1, "_USB_CLASS"))
			usb_class = udev_list_entry_get_value (e);
	}

	if (device_vendor != NULL) {
		if (device_model != NULL)
			result = g_strdup_printf ("%s - %s", device_vendor, device_model);
		else if (usb_class != NULL)
			result = g_strdup_printf ("%s - %s", device_vendor, usb_class);
	}
	if (result == NULL) {
		if (driver != NULL)
			result = g_strdup (driver);
		else
			result = g_strdup (modalias);
	}

	udev_device_unref (device);
	return result;
#else
	g_set_error_literal (error,
				AS_UTILS_ERROR,
				AS_UTILS_ERROR_FAILED,
				"Unable to determine device name: AppStream was built without udev support.");
	return NULL;
#endif
}



/**
 * as_system_info_new:
 *
 * Creates a new #AsSystemInfo.
 *
 * Returns: (transfer full): a #AsSystemInfo
 *
 * Since: 0.10
 **/
AsSystemInfo*
as_system_info_new (void)
{
	AsSystemInfo *sysinfo;
	sysinfo = g_object_new (AS_TYPE_SYSTEM_INFO, NULL);
	return AS_SYSTEM_INFO (sysinfo);
}

/**
 * as_get_current_distro_component_id:
 *
 * Returns the component-ID of the current distribution based on contents
 * of the `/etc/os-release` file.
 * This function is a shorthand for %as_distro_details_get_cid
 */
gchar*
as_get_current_distro_component_id (void)
{
	g_autoptr(AsSystemInfo) sysinfo = as_system_info_new ();
	return g_strdup (as_system_info_get_os_cid (sysinfo));

}