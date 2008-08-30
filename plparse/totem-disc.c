/* Totem Disc Content Detection
 * Copyright (C) 2004 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2004-2007 Bastien Nocera <hadess@hadess.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301  USA.
 *
 * The Totem project hereby grant permission for non-gpl compatible GStreamer
 * plugins to be used and distributed together with GStreamer and Totem. This
 * permission are above and beyond the permissions granted by the GPL license
 * Totem is covered by.
 *
 * Monday 7th February 2005: Christian Schaller: Add exception clause.
 * See license_change file for details.
 *
 */

/**
 * SECTION:totem-disc
 * @short_description: disc utility functions
 * @stability: Stable
 * @include: totem-disc.h
 *
 * This file has various different disc utility functions for getting
 * the media types and labels of discs.
 **/

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "totem-disc.h"

typedef struct _CdCache {
  /* device node and mountpoint */
  char *device, *mountpoint;
  GVolume *volume;

  char **content_types;

  GFile *iso_file;

  /* Whether we have a medium */
  guint has_medium : 1;
  /* if we're checking a media, or a dir */
  guint is_media : 1;

  /* indicates if we mounted this mountpoint ourselves or if it
   * was already mounted. */
  guint self_mounted : 1;
  guint mounted : 1;

  /* Whether it's a local ISO file */
  guint is_iso : 1;
} CdCache;

typedef struct _CdCacheCallbackData {
  CdCache *cache;
  gboolean called;
  gboolean result;
  GError *error;
} CdCacheCallbackData;

static void cd_cache_free (CdCache *cache);

static char *
totem_resolve_symlink (const char *device, GError **error)
{
  char *dir, *link;
  char *f;
  char *f1;

  f = g_strdup (device);
  while (g_file_test (f, G_FILE_TEST_IS_SYMLINK)) {
    link = g_file_read_link (f, error);
    if(link == NULL) {
      g_free (f);
      return NULL;
    }

    dir = g_path_get_dirname (f);
    f1 = g_build_filename (dir, link, NULL);
    g_free (dir);
    g_free (f);
    f = f1;
  }

  if (f != NULL) {
    GFile *file;

    file = g_file_new_for_path (f);
    f1 = g_file_get_path (file);
    g_object_unref (file);
    g_free (f);
    f = f1;
  }
  return f;
}

static gboolean
cd_cache_get_dev_from_volumes (GVolumeMonitor *mon, const char *device,
			      char **mountpoint, GVolume **v)
{
  gboolean found;
  GVolume *volume = NULL;
  GList *list, *or;

  found = FALSE;

  for (or = list = g_volume_monitor_get_volumes (mon);
       list != NULL; list = list->next) {
    char *pdev, *pdev2;

    volume = list->data;
    if (!(pdev = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE)))
      continue;
    pdev2 = totem_resolve_symlink (pdev, NULL);
    if (!pdev2) {
      g_free (pdev);
      continue;
    }
    g_free (pdev);

    if (strcmp (pdev2, device) == 0) {
      GMount *mount;

      mount = g_volume_get_mount (volume);
      if (mount) {
	GFile *file;

	file = g_mount_get_root (mount);
	*mountpoint = g_file_get_path (file);
	g_object_unref (file);
	g_object_unref (mount);
      }

      found = TRUE;
      g_object_ref (volume);
      g_free (pdev2);
      break;
    }
    g_free (pdev2);
  }
  g_list_foreach (or, (GFunc) g_object_unref, NULL);
  g_list_free (or);

  *v = volume;

  return found;
}

static gboolean
cd_cache_has_content_type (CdCache *cache, const char *content_type)
{
  guint i;

  if (cache->content_types == NULL) {
    g_message ("no content type");
    return FALSE;
  }

  for (i = 0; cache->content_types[i] != NULL; i++) {
    g_message ("type: %s", cache->content_types[i]);
    if (g_str_equal (cache->content_types[i], content_type) != FALSE)
      return TRUE;
  }
  return FALSE;
}

static char *
cd_cache_local_file_to_archive (const char *filename)
{
  char *escaped, *retval, *uri;

  uri = g_filename_to_uri (filename, NULL, NULL);
  escaped = g_uri_escape_string (uri, NULL, FALSE);
  g_free (uri);
  retval = g_strdup_printf ("archive://%s", escaped);
  g_free (escaped);

  return retval;
}

static void
cd_cache_mount_archive_callback (GObject *source_object,
				 GAsyncResult *res,
				 CdCacheCallbackData *data)
{
  data->result = g_file_mount_enclosing_volume_finish (G_FILE (source_object), res, &data->error);
  data->called = TRUE;
}

static CdCache *
cd_cache_new (const char *dev,
	      GError     **error)
{
  CdCache *cache;
  char *mountpoint = NULL, *device, *local;
  GVolumeMonitor *mon;
  GVolume *volume = NULL;
  GFile *file;
  gboolean found;

  if (dev[0] == '/') {
    local = g_strdup (dev);
    file = g_file_new_for_path (dev);
  } else {
    file = g_file_new_for_commandline_arg (dev);
    local = g_file_get_path (file);
  }

  if (local == NULL) {
    /* No error, just no cache */
    g_object_unref (file);
    return NULL;
  }

  if (g_file_test (local, G_FILE_TEST_IS_DIR) != FALSE) {
    cache = g_new0 (CdCache, 1);
    cache->mountpoint = local;
    cache->is_media = FALSE;
    cache->content_types = g_content_type_guess_for_tree (file);
    g_object_unref (file);

    return cache;
  } else if (g_file_test (local, G_FILE_TEST_IS_REGULAR)) {
    GMount *mount;
    GError *err = NULL;
    char *archive_path;

    g_object_unref (file);

    cache = g_new0 (CdCache, 1);
    cache->is_iso = TRUE;
    cache->is_media = FALSE;

    archive_path = cd_cache_local_file_to_archive (local);
    cache->device = local;

    cache->iso_file = g_file_new_for_uri (archive_path);
    g_free (archive_path);

    mount = g_file_find_enclosing_mount (cache->iso_file, NULL, &err);
    if (mount == NULL && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED)) {
      CdCacheCallbackData data;

      memset (&data, 0, sizeof(data));
      data.cache = cache;
      g_file_mount_enclosing_volume (cache->iso_file,
				     G_MOUNT_MOUNT_NONE,
				     NULL,
				     NULL,
				     (GAsyncReadyCallback) cd_cache_mount_archive_callback,
				     &data);
      while (!data.called) g_main_context_iteration (NULL, TRUE);

      if (!data.result) {
	if (data.error) {
	  g_propagate_error (error, data.error);
	} else {
	  g_set_error (error, 0, 0,
		       _("Failed to mount %s"), cache->device);
	}
	cd_cache_free (cache);
	return FALSE;
      }
    } else if (mount == NULL) {
      cd_cache_free (cache);
      return FALSE;
    } else {
      g_object_unref (mount);
    }

    cache->mountpoint = g_file_get_path (cache->iso_file);
    cache->mounted = TRUE;

    return cache;
  }

  g_object_unref (file);

  /* We have a local device
   * retrieve mountpoint and volume from gio volumes */
  device = totem_resolve_symlink (local, error);
  g_free (local);
  if (!device)
    return NULL;
  mon = g_volume_monitor_get ();
  found = cd_cache_get_dev_from_volumes (mon, device, &mountpoint, &volume);
  if (!found) {
    g_set_error (error, 0, 0,
	_("No media in drive for device '%s'"),
	device);
    g_free (device);
    return NULL;
  }

  /* create struture */
  cache = g_new0 (CdCache, 1);
  cache->device = device;
  cache->mountpoint = mountpoint;
  cache->self_mounted = FALSE;
  cache->volume = volume;
  cache->is_media = TRUE;

  {
    GMount *mount;

    mount = g_volume_get_mount (volume);
    cache->content_types = g_mount_guess_content_type_sync (mount, FALSE, NULL, NULL);
    g_object_unref (mount);
  }

  return cache;
}

static gboolean
cd_cache_has_medium (CdCache *cache)
{
  GDrive *drive;
  gboolean retval;

  if (cache->volume == NULL)
    return FALSE;

  drive = g_volume_get_drive (cache->volume);
  retval = g_drive_has_media (drive);
  g_object_unref (drive);

  return retval;
}

static gboolean
cd_cache_open_device (CdCache *cache,
		      GError **error)
{
  /* not a medium? */
  if (cache->is_media == FALSE || cache->has_medium != FALSE) {
    return TRUE;
  }

  if (cd_cache_has_medium (cache) == FALSE) {
    g_set_error (error, 0, 0,
	_("Please check that a disc is present in the drive."));
    return FALSE;
  }
  cache->has_medium = TRUE;

  return TRUE;
}

static void
cd_cache_mount_callback (GObject *source_object,
			 GAsyncResult *res,
			 CdCacheCallbackData *data)
{
  data->result = g_volume_mount_finish (data->cache->volume, res, &data->error);
  data->called = TRUE;
}

static gboolean
cd_cache_open_mountpoint (CdCache *cache,
			  GError **error)
{
  GMount *mount;
  GFile *root;

  /* already opened? */
  if (cache->mounted || cache->is_media == FALSE)
    return TRUE;

  /* check for mounting - assume we'll mount ourselves */
  if (cache->volume == NULL)
    return TRUE;

  mount = g_volume_get_mount (cache->volume);
  cache->self_mounted = (mount == NULL);

  /* mount if we have to */
  if (cache->self_mounted) {
    CdCacheCallbackData data;

    memset (&data, 0, sizeof(data));
    data.cache = cache;

    /* mount - wait for callback */
    g_volume_mount (cache->volume,
		    G_MOUNT_MOUNT_NONE,
		    NULL,
		    NULL,
		    (GAsyncReadyCallback) cd_cache_mount_callback,
		    &data);
    /* FIXME wait until it's done, any better way? */
    while (!data.called) g_main_context_iteration (NULL, TRUE);

    if (!data.result) {
      if (data.error) {
	g_propagate_error (error, data.error);
      } else {
	g_set_error (error, 0, 0,
		     _("Failed to mount %s"), cache->device);
      }
      return FALSE;
    } else {
      cache->mounted = TRUE;
      mount = g_volume_get_mount (cache->volume);
    }
  }

  if (!cache->mountpoint) {
    root = g_mount_get_root (mount);
    cache->mountpoint = g_file_get_path (root);
    g_object_unref (root);
  }

  return TRUE;
}

static void
cd_cache_unmount_callback (GObject *source_object,
			   GAsyncResult *res,
			   CdCacheCallbackData *data)
{
  data->result = g_mount_unmount_finish (G_MOUNT (source_object),
					 res, NULL);
  data->called = TRUE;
}

static void
cd_cache_free (CdCache *cache)
{
  GMount *mount;

  g_strfreev (cache->content_types);

  if (cache->iso_file) {
    mount = g_file_find_enclosing_mount (cache->iso_file,
					 NULL, NULL);
    if (mount) {
      CdCacheCallbackData data;

      memset (&data, 0, sizeof(data));

      g_mount_unmount (mount,
		       G_MOUNT_UNMOUNT_NONE,
		       NULL,
		      (GAsyncReadyCallback) cd_cache_unmount_callback,
		      &data);
      while (!data.called) g_main_context_iteration (NULL, TRUE);
      g_object_unref (mount);
    }
    g_object_unref (cache->iso_file);
  }

  /* free mem */
  if (cache->volume)
    g_object_unref (cache->volume);
  g_free (cache->mountpoint);
  g_free (cache->device);
  g_free (cache);
}

static TotemDiscMediaType
cd_cache_disc_is_cdda (CdCache *cache,
		       GError **error)
{
  /* We can't have audio CDs on disc, yet */
  if (cache->is_media == FALSE) {
    g_message ("has no media");
    return MEDIA_TYPE_DATA;
  }
  if (!cd_cache_open_device (cache, error))
    return MEDIA_TYPE_ERROR;

  if (cd_cache_has_content_type (cache, "x-content/audio-cdda") != FALSE)
    return MEDIA_TYPE_CDDA;

  return MEDIA_TYPE_DATA;
}

static TotemDiscMediaType
cd_cache_disc_is_vcd (CdCache *cache,
                      GError **error)
{
  /* open disc and open mount */
  if (!cd_cache_open_device (cache, error))
    return MEDIA_TYPE_ERROR;
  if (!cd_cache_open_mountpoint (cache, error))
    return MEDIA_TYPE_ERROR;
  if (!cache->mountpoint)
    return MEDIA_TYPE_ERROR;

  if (cd_cache_has_content_type (cache, "x-content/video-vcd") != FALSE)
    return MEDIA_TYPE_VCD;
  if (cd_cache_has_content_type (cache, "x-content/video-svcd") != FALSE)
    return MEDIA_TYPE_VCD;

  return MEDIA_TYPE_DATA;
}

static TotemDiscMediaType
cd_cache_disc_is_dvd (CdCache *cache,
		      GError **error)
{
  /* open disc, check capabilities and open mount */
  if (!cd_cache_open_device (cache, error))
    return MEDIA_TYPE_ERROR;
  if (!cd_cache_open_mountpoint (cache, error))
    return MEDIA_TYPE_ERROR;
  if (!cache->mountpoint)
    return MEDIA_TYPE_ERROR;

  if (cd_cache_has_content_type (cache, "x-content/video-dvd") != FALSE)
    return MEDIA_TYPE_DVD;

  return MEDIA_TYPE_DATA;
}

/**
 * totem_cd_mrl_from_type:
 * @scheme: a scheme (e.g. "dvd")
 * @dir: a directory URI
 *
 * Builds an MRL using the scheme @scheme and the given URI @dir,
 * taking the filename from the URI if it's a <filename>file://</filename> and just
 * using the whole URI otherwise.
 *
 * Return value: a newly-allocated string containing the MRL
 **/
char *
totem_cd_mrl_from_type (const char *scheme, const char *dir)
{
  char *retval;

  if (g_str_has_prefix (dir, "file://") != FALSE) {
    char *local;
    local = g_filename_from_uri (dir, NULL, NULL);
    retval = g_strdup_printf ("%s://%s", scheme, local);
    g_free (local);
  } else {
    retval = g_strdup_printf ("%s://%s", scheme, dir);
  }
  return retval;
}

static char *
totem_cd_dir_get_parent (const char *dir)
{
  GFile *file, *parent_file;
  char *parent;

  file = g_file_new_for_path (dir);
  parent_file = g_file_get_parent (file);
  g_object_unref (file);

  parent = g_file_get_path (parent_file);
  g_object_unref (parent_file);

  return parent;
}

/**
 * totem_cd_detect_type_from_dir:
 * @dir: a directory URI
 * @url: return location for the disc's MRL, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Detects the disc's type, given its mount directory URI. If
 * a string pointer is passed to @url, it will return the disc's
 * MRL as from totem_cd_mrl_from_type().
 *
 * Return value: #TotemDiscMediaType corresponding to the disc's type, or #MEDIA_TYPE_ERROR on failure
 **/
TotemDiscMediaType
totem_cd_detect_type_from_dir (const char *dir, char **url, GError **error)
{
  CdCache *cache;
  TotemDiscMediaType type;

  g_return_val_if_fail (dir != NULL, MEDIA_TYPE_ERROR);

  if (!(cache = cd_cache_new (dir, error)))
    return MEDIA_TYPE_ERROR;
  if ((type = cd_cache_disc_is_vcd (cache, error)) == MEDIA_TYPE_DATA &&
      (type = cd_cache_disc_is_dvd (cache, error)) == MEDIA_TYPE_DATA) {
    /* is it the directory itself? */
    char *parent;

    cd_cache_free (cache);
    parent = totem_cd_dir_get_parent (dir);
    if (!parent)
      return type;

    cache = cd_cache_new (parent, error);
    g_free (parent);
    if (!cache)
      return MEDIA_TYPE_ERROR;
    if ((type = cd_cache_disc_is_vcd (cache, error)) == MEDIA_TYPE_DATA &&
	(type = cd_cache_disc_is_dvd (cache, error)) == MEDIA_TYPE_DATA) {
      /* crap, nothing found */
      cd_cache_free (cache);
      return type;
    }
  }

  if (url == NULL) {
    cd_cache_free (cache);
    return type;
  }

  if (type == MEDIA_TYPE_DVD) {
    *url = totem_cd_mrl_from_type ("dvd", cache->mountpoint);
  } else if (type == MEDIA_TYPE_VCD) {
    *url = totem_cd_mrl_from_type ("vcd", cache->mountpoint);
  }

  cd_cache_free (cache);

  return type;
}

/**
 * totem_cd_detect_type_with_url:
 * @device: a device node path
 * @url: return location for the disc's MRL, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Detects the disc's type, given its device node path. If
 * a string pointer is passed to @url, it will return the disc's
 * MRL as from totem_cd_mrl_from_type().
 *
 * Return value: #TotemDiscMediaType corresponding to the disc's type, or #MEDIA_TYPE_ERROR on failure
 **/
TotemDiscMediaType
totem_cd_detect_type_with_url (const char *device,
    			       char      **url,
			       GError     **error)
{
  CdCache *cache;
  TotemDiscMediaType type;

  if (url != NULL)
    *url = NULL;

  if (!(cache = cd_cache_new (device, error)))
    return MEDIA_TYPE_ERROR;

  type = cd_cache_disc_is_cdda (cache, error);
  if (type == MEDIA_TYPE_ERROR && *error != NULL) {
    cd_cache_free (cache);
    return type;
  }

  if ((type == MEDIA_TYPE_DATA || type == MEDIA_TYPE_ERROR) &&
      (type = cd_cache_disc_is_vcd (cache, error)) == MEDIA_TYPE_DATA &&
      (type = cd_cache_disc_is_dvd (cache, error)) == MEDIA_TYPE_DATA) {
    /* crap, nothing found */
  }

  if (url == NULL) {
    cd_cache_free (cache);
    return type;
  }

  switch (type) {
  case MEDIA_TYPE_DVD:
    {
      const char *str;

      if (!cache->is_iso)
	str = cache->mountpoint ? cache->mountpoint : device;
      else
	str = cache->device;
      *url = totem_cd_mrl_from_type ("dvd", str);
    }
    break;
  case MEDIA_TYPE_VCD:
    {
      const char *str;

      if (!cache->is_iso)
	str = cache->mountpoint ? cache->mountpoint : device;
      else
	str = cache->device;
      *url = totem_cd_mrl_from_type ("vcd", str);
    }
    break;
  case MEDIA_TYPE_CDDA:
    {
      const char *dev;

      dev = cache->device ? cache->device : device;
      if (g_str_has_prefix (dev, "/dev/") != FALSE)
	*url = totem_cd_mrl_from_type ("cdda", dev + 5);
      else
	*url = totem_cd_mrl_from_type ("cdda", dev);
    }
    break;
  case MEDIA_TYPE_DATA:
    if (cache->is_iso) {
      type = MEDIA_TYPE_ERROR;
      /* No error, it's just not usable */
    } else {
      *url = g_strdup (cache->mountpoint);
    }
    break;
  default:
    break;
  }

  cd_cache_free (cache);

  return type;
}

/**
 * totem_cd_detect_type:
 * @device: a device node path
 * @error: return location for a #GError, or %NULL
 *
 * Detects the disc's type, given its device node path.
 *
 * Return value: #TotemDiscMediaType corresponding to the disc's type, or #MEDIA_TYPE_ERROR on failure
 **/
TotemDiscMediaType
totem_cd_detect_type (const char  *device,
		      GError     **error)
{
  return totem_cd_detect_type_with_url (device, NULL, error);
}

/**
 * totem_cd_has_medium:
 * @device: a device node path
 *
 * Returns whether the disc has a physical medium.
 *
 * Return value: %TRUE if the disc physically exists
 **/
gboolean
totem_cd_has_medium (const char *device)
{
  CdCache *cache;
  gboolean retval = TRUE;

  if (!(cache = cd_cache_new (device, NULL)))
    return TRUE;

  retval = cd_cache_has_medium (cache);
  cd_cache_free (cache);

  return retval;
}

/**
 * totem_cd_get_human_readable_name:
 * @type: a #TotemDiscMediaType
 *
 * Returns the human-readable name for the given
 * #TotemDiscMediaType.
 *
 * Return value: the disc media type's readable name, which must not be freed, or %NULL for unhandled media types
 **/
const char *
totem_cd_get_human_readable_name (TotemDiscMediaType type)
{
  switch (type)
  {
  case MEDIA_TYPE_CDDA:
    return N_("Audio CD");
  case MEDIA_TYPE_VCD:
    return N_("Video CD");
  case MEDIA_TYPE_DVD:
    return N_("DVD");
  case MEDIA_TYPE_DVB:
    return N_("Digital Television");
  default:
    g_assert_not_reached ();
  }

  return NULL;
}

/*
 * vim: sw=2 ts=8 cindent noai bs=2
 */
