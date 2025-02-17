/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "OtherCommands.hxx"
#include "Request.hxx"
#include "FileCommands.hxx"
#include "StorageCommands.hxx"
#include "CommandError.hxx"
#include "db/Uri.hxx"
#include "storage/StorageInterface.hxx"
#include "LocateUri.hxx"
#include "DetachedSong.hxx"
#include "SongPrint.hxx"
#include "TagPrint.hxx"
#include "TagStream.hxx"
#include "tag/TagHandler.hxx"
#include "TimePrint.hxx"
#include "decoder/DecoderPrint.hxx"
#include "ls.hxx"
#include "mixer/Volume.hxx"
#include "util/UriUtil.hxx"
#include "util/Error.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringAPI.hxx"
#include "fs/AllocatedPath.hxx"
#include "Stats.hxx"
#include "Permission.hxx"
#include "PlaylistFile.hxx"
#include "db/PlaylistVector.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "Idle.hxx"

#ifdef ENABLE_DATABASE
#include "DatabaseCommands.hxx"
#include "db/Interface.hxx"
#include "db/update/Service.hxx"
#endif

#include <assert.h>
#include <string.h>

static void
print_spl_list(Response &r, const PlaylistVector &list)
{
	for (const auto &i : list) {
		r.Format("playlist: %s\n", i.name.c_str());

		if (i.mtime > 0)
			time_print(r, "Last-Modified", i.mtime);
	}
}

CommandResult
handle_urlhandlers(Client &client, gcc_unused Request args, Response &r)
{
	if (client.IsLocal())
		r.Format("handler: file://\n");
	print_supported_uri_schemes(r);
	return CommandResult::OK;
}

CommandResult
handle_decoders(gcc_unused Client &client, gcc_unused Request args,
		Response &r)
{
	decoder_list_print(r);
	return CommandResult::OK;
}

CommandResult
handle_tagtypes(gcc_unused Client &client, gcc_unused Request request,
		Response &r)
{
	tag_print_types(r);
	return CommandResult::OK;
}

CommandResult
handle_kill(gcc_unused Client &client, gcc_unused Request request,
	    gcc_unused Response &r)
{
	return CommandResult::KILL;
}

CommandResult
handle_close(gcc_unused Client &client, gcc_unused Request args,
	     gcc_unused Response &r)
{
	return CommandResult::FINISH;
}

static void
print_tag(TagType type, const char *value, void *ctx)
{
	auto &r = *(Response *)ctx;

	tag_print(r, type, value);
}

CommandResult
handle_listfiles(Client &client, Request args, Response &r)
{
	/* default is root directory */
	const auto uri = args.GetOptional(0, "");

	Error error;
	const auto located_uri = LocateUri(uri, &client,
#ifdef ENABLE_DATABASE
					   nullptr,
#endif
					   error);

	switch (located_uri.type) {
	case LocatedUri::Type::UNKNOWN:
		return print_error(r, error);

	case LocatedUri::Type::ABSOLUTE:
#ifdef ENABLE_DATABASE
		/* use storage plugin to list remote directory */
		return handle_listfiles_storage(r, located_uri.canonical_uri);
#else
		r.Error(ACK_ERROR_NO_EXIST, "No database");
		return CommandResult::ERROR;
#endif

	case LocatedUri::Type::RELATIVE:
#ifdef ENABLE_DATABASE
		if (client.partition.instance.storage != nullptr)
			/* if we have a storage instance, obtain a list of
			   files from it */
			return handle_listfiles_storage(r,
							*client.partition.instance.storage,
							uri);

		/* fall back to entries from database if we have no storage */
		return handle_listfiles_db(client, r, uri);
#else
		r.Error(ACK_ERROR_NO_EXIST, "No database");
		return CommandResult::ERROR;
#endif

	case LocatedUri::Type::PATH:
		/* list local directory */
		return handle_listfiles_local(r, located_uri.canonical_uri,
					      located_uri.path);
	}

	gcc_unreachable();
}

static constexpr tag_handler print_tag_handler = {
	nullptr,
	print_tag,
	nullptr,
};

static CommandResult
handle_lsinfo_absolute(Response &r, const char *uri)
{
	if (!tag_stream_scan(uri, print_tag_handler, &r)) {
		r.Error(ACK_ERROR_NO_EXIST, "No such file");
		return CommandResult::ERROR;
	}

	return CommandResult::OK;
}

static CommandResult
handle_lsinfo_relative(Client &client, Response &r, const char *uri)
{
#ifdef ENABLE_DATABASE
	CommandResult result = handle_lsinfo2(client, uri, r);
	if (result != CommandResult::OK)
		return result;
#else
	(void)client;
#endif

	if (isRootDirectory(uri)) {
		Error error;
		const auto &list = ListPlaylistFiles(error);
		print_spl_list(r, list);
	} else {
#ifndef ENABLE_DATABASE
		r.Error(ACK_ERROR_NO_EXIST, "No database");
		return CommandResult::ERROR;
#endif
	}

	return CommandResult::OK;
}

static CommandResult
handle_lsinfo_path(Client &client, Response &r,
		   const char *path_utf8, Path path_fs)
{
	DetachedSong song(path_utf8);
	if (!song.LoadFile(path_fs)) {
		r.Error(ACK_ERROR_NO_EXIST, "No such file");
		return CommandResult::ERROR;
	}

	song_print_info(r, client.partition, song);
	return CommandResult::OK;
}

CommandResult
handle_lsinfo(Client &client, Request args, Response &r)
{
	/* default is root directory */
	auto uri = args.GetOptional(0, "");
	if (StringIsEqual(uri, "/"))
		/* this URI is malformed, but some clients are buggy
		   and use "lsinfo /" to list files in the music root
		   directory, which was never intended to work, but
		   once did; in order to retain backwards
		   compatibility, work around this here */
		uri = "";

	Error error;
	const auto located_uri = LocateUri(uri, &client,
#ifdef ENABLE_DATABASE
					   nullptr,
#endif
					   error);

	switch (located_uri.type) {
	case LocatedUri::Type::UNKNOWN:
		return print_error(r, error);

	case LocatedUri::Type::ABSOLUTE:
		return handle_lsinfo_absolute(r, located_uri.canonical_uri);

	case LocatedUri::Type::RELATIVE:
		return handle_lsinfo_relative(client, r,
					      located_uri.canonical_uri);

	case LocatedUri::Type::PATH:
		/* print information about an arbitrary local file */
		return handle_lsinfo_path(client, r, located_uri.canonical_uri,
					  located_uri.path);
	}

	gcc_unreachable();
}

#ifdef ENABLE_DATABASE

static CommandResult
handle_update(Response &r, UpdateService &update,
	      const char *uri_utf8, bool discard)
{
	unsigned ret = update.Enqueue(uri_utf8, discard);
	if (ret > 0) {
		r.Format("updating_db: %i\n", ret);
		return CommandResult::OK;
	} else {
		r.Error(ACK_ERROR_UPDATE_ALREADY, "already updating");
		return CommandResult::ERROR;
	}
}

static CommandResult
handle_update(Response &r, Database &db,
	      const char *uri_utf8, bool discard)
{
	Error error;
	unsigned id = db.Update(uri_utf8, discard, error);
	if (id > 0) {
		r.Format("updating_db: %i\n", id);
		return CommandResult::OK;
	} else if (error.IsDefined()) {
		return print_error(r, error);
	} else {
		/* Database::Update() has returned 0 without setting
		   the Error: the method is not implemented */
		r.Error(ACK_ERROR_NO_EXIST, "Not implemented");
		return CommandResult::ERROR;
	}
}

#endif

static CommandResult
handle_update(Client &client, Request args, Response &r, bool discard)
{
#ifdef ENABLE_DATABASE
	const char *path = "";

	assert(args.size <= 1);
	if (!args.IsEmpty()) {
		path = args.front();

		if (*path == 0 || StringIsEqual(path, "/"))
			/* backwards compatibility with MPD 0.15 */
			path = "";
		else if (!uri_safe_local(path)) {
			r.Error(ACK_ERROR_ARG, "Malformed path");
			return CommandResult::ERROR;
		}
	}

	UpdateService *update = client.partition.instance.update;
	if (update != nullptr)
		return handle_update(r, *update, path, discard);

	Database *db = client.partition.instance.database;
	if (db != nullptr)
		return handle_update(r, *db, path, discard);
#else
	(void)client;
	(void)args;
	(void)discard;
#endif

	r.Error(ACK_ERROR_NO_EXIST, "No database");
	return CommandResult::ERROR;
}

CommandResult
handle_update(Client &client, Request args, gcc_unused Response &r)
{
	return handle_update(client, args, r, false);
}

CommandResult
handle_rescan(Client &client, Request args, Response &r)
{
	return handle_update(client, args, r, true);
}

CommandResult
handle_setvol(Client &client, Request args, Response &r)
{
	unsigned level;
	if (!args.Parse(0, level, r, 100))
		return CommandResult::ERROR;

	if (!volume_level_change(client.partition.outputs, level)) {
		r.Error(ACK_ERROR_SYSTEM, "problems setting volume");
		return CommandResult::ERROR;
	}

	return CommandResult::OK;
}

CommandResult
handle_volume(Client &client, Request args, Response &r)
{
	int relative;
	if (!args.Parse(0, relative, r,  -100, 100))
		return CommandResult::ERROR;

	const int old_volume = volume_level_get(client.partition.outputs);
	if (old_volume < 0) {
		r.Error(ACK_ERROR_SYSTEM, "No mixer");
		return CommandResult::ERROR;
	}

	int new_volume = old_volume + relative;
	if (new_volume < 0)
		new_volume = 0;
	else if (new_volume > 100)
		new_volume = 100;

	if (new_volume != old_volume &&
	    !volume_level_change(client.partition.outputs, new_volume)) {
		r.Error(ACK_ERROR_SYSTEM, "problems setting volume");
		return CommandResult::ERROR;
	}

	return CommandResult::OK;
}

CommandResult
handle_stats(Client &client, gcc_unused Request args, Response &r)
{
	stats_print(r, client.partition);
	return CommandResult::OK;
}

CommandResult
handle_ping(gcc_unused Client &client, gcc_unused Request args,
	    gcc_unused Response &r)
{
	return CommandResult::OK;
}

CommandResult
handle_password(Client &client, Request args, Response &r)
{
	unsigned permission = 0;
	if (getPermissionFromPassword(args.front(), &permission) < 0) {
		r.Error(ACK_ERROR_PASSWORD, "incorrect password");
		return CommandResult::ERROR;
	}

	client.SetPermission(permission);

	return CommandResult::OK;
}

CommandResult
handle_config(Client &client, gcc_unused Request args, Response &r)
{
	if (!client.IsLocal()) {
		r.Error(ACK_ERROR_PERMISSION,
			"Command only permitted to local clients");
		return CommandResult::ERROR;
	}

#ifdef ENABLE_DATABASE
	const Storage *storage = client.GetStorage();
	if (storage != nullptr) {
		const auto path = storage->MapUTF8("");
		r.Format("music_directory: %s\n", path.c_str());
	}
#endif

	return CommandResult::OK;
}

CommandResult
handle_idle(Client &client, Request args, Response &r)
{
	unsigned flags = 0;
	for (const char *i : args) {
		unsigned event = idle_parse_name(i);
		if (event == 0) {
			r.FormatError(ACK_ERROR_ARG,
				      "Unrecognized idle event: %s", i);
			return CommandResult::ERROR;
		}

		flags |= event;
	}

	/* No argument means that the client wants to receive everything */
	if (flags == 0)
		flags = ~0;

	/* enable "idle" mode on this client */
	client.IdleWait(flags);

	return CommandResult::IDLE;
}
