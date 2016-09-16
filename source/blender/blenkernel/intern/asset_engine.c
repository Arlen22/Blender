/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/asset_engine.c
 *  \ingroup bke
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "RNA_access.h"
#include "RNA_types.h"

#include "BLT_translation.h"

#include "BLI_fileops_types.h"
#include "BLI_hash_mm2a.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "PIL_time.h"

#include "BKE_asset_engine.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_report.h"

#include "IMB_imbuf.h"

#include "DNA_space_types.h"

#ifdef WITH_PYTHON
#include "BPY_extern.h"
#endif

/* Asset engine types (none intern!). */

ListBase asset_engines = {NULL, NULL};

void BKE_asset_engines_init(void)
{
	/* We just add a dummy engine, which 'is' our intern filelisting code from space_file! */
	AssetEngineType *aet = MEM_callocN(sizeof(*aet), __func__);

	BLI_strncpy(aet->idname, AE_FAKE_ENGINE_ID, sizeof(aet->idname));
	BLI_strncpy(aet->name, "None", sizeof(aet->name));

	BLI_addhead(&asset_engines, aet);
}

void BKE_asset_engines_exit(void)
{
	AssetEngineType *type, *next;

	for (type = asset_engines.first; type; type = next) {
		next = type->next;

		BLI_remlink(&asset_engines, type);

		if (type->ext.free) {
			type->ext.free(type->ext.data);
		}

		MEM_freeN(type);
	}
}

AssetEngineType *BKE_asset_engines_find(const char *idname)
{
	AssetEngineType *type;

	type = BLI_findstring(&asset_engines, idname, offsetof(AssetEngineType, idname));

	return type;
}

AssetEngineType *BKE_asset_engines_get_default(char *r_idname, const size_t len)
{
	AssetEngineType *type = asset_engines.first;

	BLI_assert(type);

	if (r_idname) {
		BLI_strncpy(r_idname, type->idname, len);
	}

	return type;
}

/* Asset engine instances. */

/* Create, Free */

AssetEngine *BKE_asset_engine_create(AssetEngineType *type, ReportList *reports)
{
	AssetEngine *engine;

	BLI_assert(type);

	engine = MEM_callocN(sizeof(AssetEngine), __func__);
	engine->type = type;
	engine->refcount = 1;

	/* initialize error reports */
	if (reports) {
		engine->reports = reports; /* must be initialized already */
	}
	else {
		engine->reports = MEM_mallocN(sizeof(ReportList), __func__);
		BKE_reports_init(engine->reports, RPT_STORE | RPT_FREE);
	}

	return engine;
}

/** Shalow copy only (i.e. memory is 100% shared, just increases refcount). */
AssetEngine *BKE_asset_engine_copy(AssetEngine *engine)
{
	engine->refcount++;
	return engine;
}

void BKE_asset_engine_free(AssetEngine *engine)
{
	if (engine->refcount-- == 1) {
#ifdef WITH_PYTHON
		if (engine->py_instance) {
			BPY_DECREF_RNA_INVALIDATE(engine->py_instance);
		}
#endif

		if (engine->properties) {
			IDP_FreeProperty(engine->properties);
			MEM_freeN(engine->properties);
		}

		if (engine->reports && (engine->reports->flag & RPT_FREE)) {
			BKE_reports_clear(engine->reports);
			MEM_freeN(engine->reports);
		}

		MEM_freeN(engine);
	}
}


/* API helpers. */

static void asset_engine_load_pre(AssetEngine *engine, AssetUUIDList *r_uuids, FileDirEntryArr *r_entries)
{
	FileDirEntry *en;
	AssetUUID *uuid;
	const int nbr_entries = r_entries->nbr_entries ? r_entries->nbr_entries : r_uuids->nbr_uuids;

	if (r_entries->nbr_entries) {
		BLI_assert(r_uuids->uuids == NULL);
		r_uuids->uuids = MEM_mallocN(sizeof(*r_uuids->uuids) * nbr_entries, __func__);
		r_uuids->nbr_uuids = nbr_entries;
		r_uuids->asset_engine_version = engine->type->version;

		for (en = r_entries->entries.first, uuid = r_uuids->uuids; en; en = en->next, uuid++) {
			FileDirEntryVariant *var = BLI_findlink(&en->variants, en->act_variant);

			memcpy(uuid->uuid_asset, en->uuid, sizeof(uuid->uuid_asset));

			BLI_assert(var);
			memcpy(uuid->uuid_variant, var->uuid, sizeof(uuid->uuid_variant));

			memcpy(uuid->uuid_revision, en->entry->uuid, sizeof(uuid->uuid_revision));
		}
	}

	BKE_filedir_entryarr_clear(r_entries);

	if (!engine->type->load_pre(engine, r_uuids, r_entries)) {
		/* If load_pre returns false (i.e. fails), clear all paths! */
		/* TODO: report!!! */
		BKE_filedir_entryarr_clear(r_entries);

		MEM_freeN(r_uuids->uuids);
		r_uuids->uuids = NULL;
		r_uuids->nbr_uuids = 0;
		return;
	}

	/* load_pre may change things, we have to rebuild our uuids list from returned entries. */
	r_entries->nbr_entries = r_uuids->nbr_uuids = BLI_listbase_count(&r_entries->entries);
	r_uuids->uuids = MEM_reallocN(r_uuids->uuids, sizeof(*r_uuids->uuids) * r_uuids->nbr_uuids);
	for (en = r_entries->entries.first, uuid = r_uuids->uuids; en; en = en->next, uuid++) {
		FileDirEntryVariant *var;
		FileDirEntryRevision *rev;

		memcpy(uuid->uuid_asset, en->uuid, sizeof(uuid->uuid_asset));

		var = BLI_findlink(&en->variants, en->act_variant);
		BLI_assert(var);
		memcpy(uuid->uuid_variant, var->uuid, sizeof(uuid->uuid_variant));

		rev = BLI_findlink(&var->revisions, var->act_revision);
		BLI_assert(rev);
		memcpy(uuid->uuid_revision, rev->uuid, sizeof(uuid->uuid_revision));
	}
}

/** Call load_pre for given entries, and return new uuids/entries. */
AssetUUIDList *BKE_asset_engine_entries_load_pre(AssetEngine *engine, FileDirEntryArr *r_entries)
{
	AssetUUIDList *uuids = MEM_callocN(sizeof(*uuids), __func__);

	asset_engine_load_pre(engine, uuids, r_entries);

	return uuids;
}

/** Call load_pre for given uuids, and return new uuids/entries. */
FileDirEntryArr *BKE_asset_engine_uuids_load_pre(AssetEngine *engine, AssetUUIDList *r_uuids)
{
	FileDirEntryArr *entries = MEM_callocN(sizeof(*entries), __func__);

	asset_engine_load_pre(engine, r_uuids, entries);

	return entries;
}

/* FileDirxxx handling. */

void BKE_filedir_revision_free(FileDirEntryRevision *rev)
{
	if (rev->comment) {
		MEM_freeN(rev->comment);
	}
	MEM_freeN(rev);
}

void BKE_filedir_variant_free(FileDirEntryVariant *var)
{
	if (var->name) {
		MEM_freeN(var->name);
	}
	if (var->description) {
		MEM_freeN(var->description);
	}

	if (!BLI_listbase_is_empty(&var->revisions)) {
		FileDirEntryRevision *rev, *rev_next;

		for (rev = var->revisions.first; rev; rev = rev_next) {
			rev_next = rev->next;
			BKE_filedir_revision_free(rev);
		}

		BLI_listbase_clear(&var->revisions);
	}
	MEM_freeN(var);
}

void BKE_filedir_entry_clear(FileDirEntry *entry)
{
	if (entry->name) {
		MEM_freeN(entry->name);
	}
	if (entry->description) {
		MEM_freeN(entry->description);
	}
	if (entry->relpath) {
		MEM_freeN(entry->relpath);
	}
	if (entry->image) {
		IMB_freeImBuf(entry->image);
	}
	/* For now, consider FileDirEntryRevision::poin as not owned here, so no need to do anything about it */

	if (!BLI_listbase_is_empty(&entry->variants)) {
		FileDirEntryVariant *var, *var_next;

		for (var = entry->variants.first; var; var = var_next) {
			var_next = var->next;
			BKE_filedir_variant_free(var);
		}

		BLI_listbase_clear(&entry->variants);
	}
	else if (entry->entry){
		MEM_freeN(entry->entry);
	}

	memset(entry, 0, sizeof(*entry));
}

void BKE_filedir_entry_free(FileDirEntry *entry)
{
	BKE_filedir_entry_clear(entry);
	MEM_freeN(entry);
}

/** Perform and return a full (deep) duplicate of given entry. */
FileDirEntry *BKE_filedir_entry_copy(FileDirEntry *entry)
{
	FileDirEntry *entry_new = MEM_dupallocN(entry);

	if (entry->name) {
		entry_new->name = MEM_dupallocN(entry->name);
	}
	if (entry->description) {
		entry_new->description = MEM_dupallocN(entry->description);
	}
	if (entry->relpath) {
		entry_new->relpath = MEM_dupallocN(entry->relpath);
	}
	if (entry->image) {
		entry_new->image = IMB_dupImBuf(entry->image);
	}
	/* For now, consider FileDirEntryRevision::poin as not owned here, so no need to do anything about it */

	entry_new->entry = NULL;
	if (!BLI_listbase_is_empty(&entry->variants)) {
		FileDirEntryVariant *var;
		int act_var;

		BLI_listbase_clear(&entry_new->variants);
		for (act_var = 0, var = entry->variants.first; var; act_var++, var = var->next) {
			FileDirEntryVariant *var_new = MEM_dupallocN(var);
			FileDirEntryRevision *rev;
			const bool is_act_var = (act_var == entry->act_variant);
			int act_rev;

			if (var->name) {
				var_new->name = MEM_dupallocN(var->name);
			}
			if (var->description) {
				var_new->description = MEM_dupallocN(var->description);
			}

			BLI_listbase_clear(&var_new->revisions);
			for (act_rev = 0, rev = var->revisions.first; rev; act_rev++, rev = rev->next) {
				FileDirEntryRevision *rev_new = MEM_dupallocN(rev);
				const bool is_act_rev = (act_rev == var->act_revision);

				if (rev->comment) {
					rev_new->comment = MEM_dupallocN(rev->comment);
				}

				BLI_addtail(&var_new->revisions, rev_new);

				if (is_act_var && is_act_rev) {
					entry_new->entry = rev_new;
				}
			}

			BLI_addtail(&entry_new->variants, var_new);
		}

	}
	else if (entry->entry){
		entry_new->entry = MEM_dupallocN(entry->entry);
	}

	BLI_assert(entry_new->entry != NULL);

	/* TODO: tags! */

	return entry_new;
}

void BKE_filedir_entryarr_clear(FileDirEntryArr *array)
{
	FileDirEntry *entry, *entry_next;

	for (entry = array->entries.first; entry; entry = entry_next) {
		entry_next = entry->next;
		BKE_filedir_entry_free(entry);
	}
	BLI_listbase_clear(&array->entries);
    array->nbr_entries = 0;
	array->nbr_entries_filtered = 0;
}

/* Various helpers */
unsigned int BKE_asset_uuid_hash(const void *key)
{
	return BLI_hash_mm2((const unsigned char *)key, sizeof(AssetUUID), 0);
}

bool BKE_asset_uuid_cmp(const void *a, const void *b)
{
	const AssetUUID *uuid1 = a;
	const AssetUUID *uuid2 = b;
	return !ASSETUUID_COMPARE(uuid1, uuid2);  /* Expects false when compared equal... */
}

void BKE_asset_uuid_print(const AssetUUID *uuid)
{
	/* TODO print nicer (as 128bit hexadecimal...). */
	printf("[%d,%d,%d,%d][%d,%d,%d,%d][%d,%d,%d,%d]\n",
	       uuid->uuid_asset[0], uuid->uuid_asset[1], uuid->uuid_asset[2], uuid->uuid_asset[3],
	       uuid->uuid_variant[0], uuid->uuid_variant[1], uuid->uuid_variant[2], uuid->uuid_variant[3],
	       uuid->uuid_revision[0], uuid->uuid_revision[1], uuid->uuid_revision[2], uuid->uuid_revision[3]);
}
