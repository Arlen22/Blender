# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

# Note: This will be a simple addon later, but until it gets to master, it's simpler to have it
#       as a startup module!

import bpy
from bpy.types import (
        AssetEngine,
        Panel,
        PropertyGroup,
        UIList,
        )
from bpy.props import (
        StringProperty,
        BoolProperty,
        IntProperty,
        FloatProperty,
        EnumProperty,
        CollectionProperty,
        )

import binascii
import concurrent.futures as futures
import asyncio
import hashlib
import json
import os
import stat
import struct
import time
import random


AMBER_DB_NAME = "__amber_db.json"
AMBER_DBK_VERSION = "version"


##########
# Helpers.

# Notes about UUIDs:
#    * UUID of an asset/variant/revision is computed once at its creation! Later changes to data do not affect it.
#    * Collision, for unlikely it is, may happen across different repositories...
#      Doubt this will be practical issue though.
#    * We keep eight first bytes of 'clear' identifier, to (try to) keep some readable uuid.

def _uuid_gen_single(used_uuids, uuid_root, h, str_arg):
    h.update(str_arg.encode())
    uuid = uuid_root + h.digest()
    uuid = uuid[:23].replace(b'\0', b'\1')  # No null chars, RNA 'bytes' use them as in regular strings... :/
    if uuid not in used_uuids:  # *Very* likely, but...
        used_uuids.add(uuid)
        return uuid
    return None


def _uuid_gen(used_uuids, uuid_root, bytes_seed, *str_args):
    h = hashlib.md5(bytes_seed)
    for arg in str_args:
        uuid = _uuid_gen_single(used_uuids, uuid_root, h, arg)
        if uuid is not None:
            return uuid
    # This is a fallback in case we'd get a collision... Should never be needed in real life!
    for i in range(100000):
        uuid = _uuid_gen_single(used_uuids, uuid_root, h, i.to_bytes(4, 'little'))
        if uuid is not None:
            return uuid
    return None  # If this happens...


def uuid_asset_gen(used_uuids, path_db, name, tags):
    uuid_root = name.encode()[:8] + b'|'
    return _uuid_gen_single(used_uuids, uuid_root, path_db.encode(), name, *tags)


def uuid_variant_gen(used_uuids, asset_uuid, name):
    uuid_root = name.encode()[:8] + b'|'
    return _uuid_gen_single(used_uuids, uuid_root, asset_uuid, name)


def uuid_revision_gen(used_uuids, variant_uuid, number, size, time):
    uuid_root = str(number).encode() + b'|'
    return _uuid_gen_single(used_uuids, uuid_root, variant_uuid, str(number), str(size), str(timestamp))


def uuid_unpack_bytes(uuid_bytes):
    return struct.unpack("!iiii", uuid_bytes.ljust(16, b'\0'))


def uuid_unpack(uuid_hexstr):
    return uuid_unpack_bytes(binascii.unhexlify(uuid_hexstr))


def uuid_unpack_asset(uuid_repo_hexstr, uuid_asset_hexstr):
    return uuid_unpack_bytes(binascii.unhexlify(uuid_repo_hexstr).ljust(8, b'\0') +
                             binascii.unhexlify(uuid_asset_hexstr).ljust(8, b'\0'))


def uuid_pack(uuid_iv4):
    print(uuid_iv4)
    return binascii.hexlify(struct.pack("!iiii", *uuid_iv4))


# XXX Hack, once this becomes a real addon we'll just use addons' config system, for now store that in some own config.
amber_repos_path = os.path.join(bpy.utils.user_resource('CONFIG', create=True), "amber_repos.json")
amber_repos = None
if not os.path.exists(amber_repos_path):
    with open(amber_repos_path, 'w') as ar_f:
        json.dump({}, ar_f)
with open(amber_repos_path, 'r') as ar_f:
    amber_repos = {uuid_unpack(uuid): path for uuid, path in json.load(ar_f).items()}
assert(amber_repos != None)


def save_amber_repos():
    ar = {uuid_pack(uuid).decode(): path for uuid, path in amber_repos.items()}
    with open(amber_repos_path, 'w') as ar_f:
        json.dump(ar, ar_f)


#############
# Amber Jobs.
class AmberJob:
    def __init__(self, job_id):
        loop = asyncio.get_event_loop()
        self.job_id = job_id
        self.job_future = loop.create_future()
        self.status = {'VALID'}
        self.progress = 0.0

    @staticmethod
    def async_looper(func):
        """Defines a simple wrapper around the function that executes it before stepping a bit asyncio loop."""
        def wrapper(*args, **kwargs):
            loop = asyncio.get_event_loop()
            print("proceed....")
            func(*args, **kwargs)
            print("kickstep")
            # That's the trick - since asyncio loop is not the main loop, we cannot call run_forever, or we would
            # never get back our thread (not until something in asyncio loop itself calls stop(), at least).
            # So we schedule ourselves the stop call, effectively leading to 'stepping' asyncio loop.
            # This relies on the fact that main loop (aka Blender) calls an @AmberJob.async_looper func often enough!
            loop.call_soon(loop.stop)
            loop.run_forever()
        return wrapper


class AmberJobList(AmberJob):
    @staticmethod
    async def ls_repo(job_future, db_path):
        if job_future.cancelled():
            return None

        loop = asyncio.get_event_loop()
        repo = None
        with open(db_path, 'r') as db_f:
            repo = await loop.run_in_executor(None, json.load, db_f)
        if isinstance(repo, dict):
            repo_ver = repo.get(AMBER_DBK_VERSION, "")
            if repo_ver != "1.0.1":
                # Unsupported...
                print("WARNING: unsupported Amber repository version '%s'." % repo_ver)
                repo = None
        else:
            repo = None
        if repo is not None:
            # Convert hexa string to array of four uint32...
            # XXX will have to check endianess mess here, for now always use same one ('network' one).
            repo_uuid = repo["uuid"]
            repo["uuid"] = uuid_unpack(repo_uuid)
            new_entries = {}
            for euuid, e in repo["entries"].items():
                new_variants = {}
                for vuuid, v in e["variants"].items():
                    new_revisions = {}
                    for ruuid, r in v["revisions"].items():
                        new_revisions[uuid_unpack(ruuid)] = r
                    new_variants[uuid_unpack(vuuid)] = v
                    v["revisions"] = new_revisions
                    ruuid = v["revision_default"]
                    v["revision_default"] = uuid_unpack(ruuid)
                new_entries[uuid_unpack_asset(repo_uuid, euuid)] = e
                e["variants"] = new_variants
                vuuid = e["variant_default"]
                e["variant_default"] = uuid_unpack(vuuid)
            repo["entries"] = new_entries
        #~ print(repo)
        return repo

    @staticmethod
    async def ls(job_future, path):
        if job_future.cancelled():
            return None

        print(1)
        loop = asyncio.get_event_loop()
        repo = None
        ret = [".."]
        print(2)
        tmp = await loop.run_in_executor(None, os.listdir, path)
        print(3)
        if AMBER_DB_NAME in tmp:
            # That dir is an Amber repo, we only list content define by our amber 'db'.
            repo = await AmberJobList.ls_repo(job_future, os.path.join(path, AMBER_DB_NAME))
        if repo is None:
            ret += tmp
        #~ time.sleep(0.1)  # 100% Artificial Lag (c)
        print(4)
        return ret, repo

    @staticmethod
    async def stat(job_future, root, path):
        if job_future.cancelled():
            return None

        loop = asyncio.get_event_loop()
        st = await loop.run_in_executor(None, os.lstat, root + path)
        #~ time.sleep(0.1)  # 100% Artificial Lag (c)
        return path, (stat.S_ISDIR(st.st_mode), st.st_size, st.st_mtime)

    @AmberJob.async_looper
    def start(self):
        self.nbr = 0
        self.tot = 0
        self.ls_task = asyncio.ensure_future(self.ls(self.job_future, self.root))
        self.status = {'VALID', 'RUNNING'}

    @AmberJob.async_looper
    def update(self, repository, dirs):
        if self.job_future.cancelled():
            self.cancel()
            return

        self.status = {'VALID', 'RUNNING'}
        if self.ls_task is not None:
            if not self.ls_task.done():
                return
            paths, repo = self.ls_task.result()
            self.ls_task = None
            self.tot = len(paths)
            repository.clear()
            dirs.clear()
            if repo is not None:
                repository.update(repo)
            for p in paths:
                self.stat_tasks.add(asyncio.ensure_future(self.stat(self.job_future, self.root, p)))

        done = set()
        for tsk in self.stat_tasks:
            if tsk.done():
                path, (is_dir, size, timestamp) = tsk.result()
                self.nbr += 1
                if is_dir:
                    # We only list dirs from real file system.
                    uuid = uuid_unpack_bytes((path.encode()[:8] + b"|" + self.nbr.to_bytes(4, 'little')))
                    dirs.append((path, size, timestamp, uuid))
                done.add(tsk)
        self.stat_tasks -= done

        self.progress = self.nbr / self.tot
        if not self.stat_tasks and self.ls_task is None:
            self.status = {'VALID'}

    def cancel(self):
        self.job_future.cancel()
        if self.ls_task is not None and not self.ls_task.done():
            self.ls_task.cancel()
        for tsk in self.stat_tasks:
            if not tsk.done():
                tsk.cancel()
        self.status = {'VALID'}

    def __init__(self, job_id, root):
        super().__init__(job_id)
        self.root = root

        self.ls_task = None
        self.stat_tasks = set()

        self.start()

    def __del__(self):
        self.cancel()


"""
class AmberJobPreviews(AmberJob):
    @staticmethod
    def preview(uuid):
        time.sleep(0.1)  # 100% Artificial Lag (c)
        w = random.randint(2, 8)
        h = random.randint(2, 8)
        return [w, h, [random.getrandbits(32) for i in range(w * h)]]

    def start(self, uuids):
        self.nbr = 0
        self.preview_tasks = {uuid.uuid_asset[:]: self.executor.submit(self.preview, uuid.uuid_asset[:]) for uuid in uuids.uuids}
        self.tot = len(self.preview_tasks)
        self.status = {'VALID', 'RUNNING'}

    def update(self, uuids):
        self.status = {'VALID', 'RUNNING'}

        uuids = {uuid.uuid_asset[:]: uuid for uuid in uuids.uuids}

        new_uuids = set(uuids)
        old_uuids = set(self.preview_tasks)
        del_uuids = old_uuids - new_uuids
        new_uuids -= old_uuids

        for uuid in del_uuids:
            self.preview_tasks[uuid].cancel()
            del self.preview_tasks[uuid]

        for uuid in new_uuids:
            self.preview_tasks[uuid] = self.executor.submit(self.preview, uuid)

        self.tot = len(self.preview_tasks)
        self.nbr = 0

        done_uuids = set()
        for uuid, tsk in self.preview_tasks.items():
            if tsk.done():
                w, h, pixels = tsk.result()
                uuids[uuid].preview_size = (w, h)
                uuids[uuid].preview_pixels = pixels
                self.nbr += 1
                done_uuids.add(uuid)

        for uuid in done_uuids:
            del self.preview_tasks[uuid]

        self.progress = self.nbr / self.tot
        if not self.preview_tasks:
            self.status = {'VALID'}

    def __init__(self, executor, job_id, uuids):
        super().__init__(executor, job_id)
        self.preview_tasks = {}

        self.start(uuids)

    def __del__(self):
        # Avoid useless work!
        for tsk in self.preview_tasks.values():
            tsk.cancel()
"""


###########################
# Main Asset Engine class.
class AmberAIOTag(PropertyGroup):
    name = StringProperty(name="Name", description="Tag name")
    priority = IntProperty(name="Priority", default=0, description="Tag priority")

    def include_update(self, context):
        if self.use_include:
            self.use_exclude = False
        context.space_data.asset_engine.is_dirty_filtering = True
    use_include = BoolProperty(name="Include", default=False, description="This tag must exist in filtered items",
                               update=include_update)

    def exclude_update(self, context):
        if self.use_exclude:
            self.use_include = False
        context.space_data.asset_engine.is_dirty_filtering = True
    use_exclude = BoolProperty(name="Exclude", default=False, description="This tag must not exist in filtered items",
                               update=exclude_update)


class AssetEngineAmberAIO(AssetEngine):
    bl_label = "AmberAIO"
    bl_version = (0 << 16) + (0 << 8) + 4  # Usual maj.min.rev version scheme...

    tags = CollectionProperty(name="Tags", type=AmberAIOTag, description="Filtering tags")
    active_tag_index = IntProperty(name="Active Tag", options={'HIDDEN'})

    def __init__(self):
        self.executor = futures.ThreadPoolExecutor(8)  # Using threads for now, if issues arise we'll switch to process.
        self.jobs = {}
        self.repos = {}

        self.reset()

        self.job_uuid = 1

    def __del__(self):
        # XXX This errors, saying self has no executor attribute... Suspect some py/RNA funky game. :/
        #     Even though it does not seem to be an issue, this is not nice and shall be fixed somehow.
        # XXX This is still erroring... Looks like we should rather have a 'remove' callback or so. :|
        #~ executor = getattr(self, "executor", None)
        #~ if executor is not None:
            #~ executor.shutdown(wait=False)
        pass

    ########## Various helpers ##########
    def reset(self):
        print("AmberAIO Reset!")
        self.jobs = {}
        self.root = ""
        self.repo = {}
        self.dirs = []

        self.sortedfiltered = []

    def entry_from_uuid(self, entries, euuid, vuuid, ruuid):
        e = self.repo["entries"][euuid]
        entry = entries.entries.add()
        entry.uuid = euuid
        entry.name = e["name"]
        entry.description = e["description"]
        entry.type = {e["file_type"]}
        entry.blender_type = e["blen_type"]
        act_rev = None
        if vuuid == (0, 0, 0, 0):
            for vuuid, v in e["variants"].items():
                variant = entry.variants.add()
                variant.uuid = vuuid
                variant.name = v["name"]
                variant.description = v["description"]
                if vuuid == e["variant_default"]:
                    entry.variants.active = variant
                for ruuid, r in v["revisions"].items():
                    revision = variant.revisions.add()
                    revision.uuid = ruuid
                    #~ revision.comment = r["comment"]
                    revision.size = r["size"]
                    revision.timestamp = r["timestamp"]
                    if ruuid == v["revision_default"]:
                        variant.revisions.active = revision
                        if vuuid == e["variant_default"]:
                            act_rev = r
        else:
            v = e["variants"][vuuid]
            variant = entry.variants.add()
            variant.uuid = vuuid
            variant.name = v["name"]
            variant.description = v["description"]
            entry.variants.active = variant
            if ruuid == (0, 0, 0, 0):
                for ruuid, r in v["revisions"].items():
                    revision = variant.revisions.add()
                    revision.uuid = ruuid
                    #~ revision.comment = r["comment"]
                    revision.size = r["size"]
                    revision.timestamp = r["timestamp"]
                    if ruuid == v["revision_default"]:
                        variant.revisions.active = revision
                        act_rev = r
            else:
                r = v["revisions"][ruuid]
                revision = variant.revisions.add()
                revision.uuid = ruuid
                #~ revision.comment = r["comment"]
                revision.size = r["size"]
                revision.timestamp = r["timestamp"]
                variant.revisions.active = revision
                act_rev = r
        if act_rev:
            entry.relpath = act_rev["path"]
#        print("added entry for", entry.relpath)

    def pretty_version(self, v=None):
        if v is None:
            v = self.bl_version
        return "%d.%d.%d" % ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)

    ########## PY-API only ##########
    # UI header
    def draw_header(self, layout, context):
        st = context.space_data
        params = st.params

        # can be None when save/reload with a file selector open
        if params:
            is_lib_browser = params.use_library_browsing

            layout.prop(params, "display_type", expand=True, text="")
            layout.prop(params, "display_size", text="")
            layout.prop(params, "sort_method", expand=True, text="")

            layout.prop(params, "show_hidden", text="", icon='FILE_HIDDEN')
            layout.prop(params, "use_filter", text="", icon='FILTER')

            row = layout.row(align=True)
            row.active = params.use_filter

            if params.filter_glob:
                #if st.active_operator and hasattr(st.active_operator, "filter_glob"):
                #    row.prop(params, "filter_glob", text="")
                row.label(params.filter_glob)
            else:
                row.prop(params, "use_filter_blender", text="")
                row.prop(params, "use_filter_backup", text="")
                row.prop(params, "use_filter_image", text="")
                row.prop(params, "use_filter_movie", text="")
                row.prop(params, "use_filter_script", text="")
                row.prop(params, "use_filter_font", text="")
                row.prop(params, "use_filter_sound", text="")
                row.prop(params, "use_filter_text", text="")

            if is_lib_browser:
                row.prop(params, "use_filter_blendid", text="")
                if (params.use_filter_blendid) :
                    row.separator()
                    row.prop(params, "filter_id_category", text="")

            row.separator()
            row.prop(params, "filter_search", text="", icon='VIEWZOOM')

    ########## C (RNA) API ##########
    def status(self, job_id):
        if job_id:
            job = self.jobs.get(job_id, None)
            return job.status if job is not None else set()
        return {'VALID'}

    def progress(self, job_id):
        if job_id:
            job = self.jobs.get(job_id, None)
            return job.progress if job is not None else 0.0
        progress = 0.0
        nbr_jobs = 0
        for job in self.jobs.values():
            if 'RUNNING' in job.status:
                nbr_jobs += 1
                progress += job.progress
        return progress / nbr_jobs if nbr_jobs else 0.0

    def kill(self, job_id):
        if job_id:
            self.jobs.pop(job_id, None)
            return
        self.jobs.clear()

    def list_dir(self, job_id, entries):
        job = self.jobs.get(job_id, None)
        #~ print(entries.root_path, job_id, job)
        if job is not None and isinstance(job, AmberJobList):
            if job.root != entries.root_path:
                self.reset()
                self.jobs[job_id] = AmberJobList(job_id, entries.root_path)
                self.root = entries.root_path
            else:
                job.update(self.repo, self.dirs)
        elif self.root != entries.root_path:
            self.reset()
            job_id = self.job_uuid
            self.job_uuid += 1
            self.jobs[job_id] = AmberJobList(job_id, entries.root_path)
            self.root = entries.root_path
        if self.repo:
            uuid_repo = self.repo["uuid"]
            if amber_repos.get(uuid_repo, None) != self.root:
                amber_repos[uuid_repo] = self.root  # XXX Not resistant to uuids collisions (use a set instead)...
                save_amber_repos()
            self.repos[uuid_repo] = self.repo
            entries.nbr_entries = len(self.repo["entries"])
            valid_tags = set()
            for name, prio in sorted(self.repo["tags"].items(), key=lambda i: i[1], reverse=True):
                tag = self.tags.get(name)
                if tag is None:
                    tag = self.tags.add()
                    tag.name = name
                tag.priority = prio
                valid_tags.add(name)
            for name in (set(self.tags.keys()) - valid_tags):
                del self.tags[name]
        else:
            entries.nbr_entries = len(self.dirs)
            self.tags.clear()
        return job_id

    def update_check(self, job_id, uuids):
        # do nothing for now, no need to use actual job...
        if uuids.asset_engine_version != self.bl_version:
            print("Updating asset uuids from Amber v.%s to amber v.%s" %
                  (self.pretty_version(uuids.asset_engine_version), self.pretty_version()))
        for uuid in uuids.uuids:
            repo_uuid = uuid.uuid_asset[:2] + (0, 0)
            if repo_uuid not in amber_repos or not os.path.exists(os.path.join(amber_repos[repo_uuid], AMBER_DB_NAME)):
                uuid.is_asset_missing = True
                continue
            # Here in theory we'd reload given repo (async process) and check for asset's status...
            uuid.use_asset_reload = True
        return self.job_id_invalid

    """
    def previews_get(self, job_id, uuids):
        pass
        job = self.jobs.get(job_id, None)
        #~ print(entries.root_path, job_id, job)
        if job is not None and isinstance(job, AmberJobPreviews):
            job.update(uuids)
        else:
            job_id = self.job_uuid
            self.job_uuid += 1
            self.jobs[job_id] = AmberJobPreviews(self.executor, job_id, uuids)
        return job_id
    """

    def load_pre(self, uuids, entries):
        # Not quite sure this engine will need it in the end, but for sake of testing...
        if uuids.asset_engine_version != self.bl_version:
            print("Updating asset uuids from Amber v.%s to amber v.%s" %
                  (self.pretty_version(uuids.asset_engine_version), self.pretty_version()))
#            print(entries.entries[:])
        for uuid in uuids.uuids:
            repo_uuid = tuple(uuid.uuid_asset)[:2] + (0, 0)
            assert(repo_uuid in amber_repos)
            repo = self.repos.get(repo_uuid, None)
            if repo is None:
                repo = self.repos[repo_uuid] = AmberJobList.ls_repo(os.path.join(amber_repos[repo_uuid], AMBER_DB_NAME))
            euuid = tuple(uuid.uuid_asset)
            vuuid = tuple(uuid.uuid_variant)
            ruuid = tuple(uuid.uuid_revision)
            e = repo["entries"][euuid]
            v = e["variants"][vuuid]
            r = v["revisions"][ruuid]

            entry = entries.entries.add()
            entry.type = {e["file_type"]}
            entry.blender_type = e["blen_type"]
            # archive part not yet implemented!
            entry.relpath = os.path.join(amber_repos[repo_uuid], r["path"])
#                print("added entry for", entry.relpath)
            entry.uuid = euuid
            var = entry.variants.add()
            var.uuid = vuuid
            rev = var.revisions.add()
            rev.uuid = ruuid
            var.revisions.active = rev
            entry.variants.active = var
        entries.root_path = ""
        return True

    def check_dir(self, entries, do_change):
        # Stupid code just for test...
        #~ if do_change:
        #~     entries.root_path = entries.root_path + "../"
        #~     print(entries.root_path)
        return True

    def sort_filter(self, use_sort, use_filter, params, entries):
#        print(use_sort, use_filter)
        if use_filter:
            filter_search = params.filter_search
            self.sortedfiltered.clear()
            if self.repo:
                if params.use_filter:
                    file_type = set()
                    blen_type = set()
                    tags_incl = {t.name for t in self.tags if t.use_include}
                    tags_excl = {t.name for t in self.tags if t.use_exclude}
                    if params.use_filter_image:
                        file_type.add('IMAGE')
                    if params.use_filter_blender:
                        file_type.add('BLENDER')
                    if params.use_filter_backup:
                        file_type.add('BACKUP')
                    if params.use_filter_movie:
                        file_type.add('MOVIE')
                    if params.use_filter_script:
                        file_type.add('SCRIPT')
                    if params.use_filter_font:
                        file_type.add('FONT')
                    if params.use_filter_sound:
                        file_type.add('SOUND')
                    if params.use_filter_text:
                        file_type.add('TEXT')
                    if params.use_filter_blendid and params.use_library_browsing:
                        file_type.add('BLENLIB')
                        blen_type = params.filter_id

                for key, val in self.repo["entries"].items():
                    if filter_search and filter_search not in (val["name"] + val["description"]):
                        continue
                    if params.use_filter:
                        if val["file_type"] not in file_type:
                            continue
                        if params.use_library_browsing and val["blen_type"] not in blen_type:
                            continue
                        if tags_incl or tags_excl:
                            tags = set(val["tags"])
                            if tags_incl and ((tags_incl & tags) != tags_incl):
                                continue
                            if tags_excl and (tags_excl & tags):
                                continue
                    self.sortedfiltered.append((key, val))

            elif self.dirs:
                for path, size, timestamp, uuid in self.dirs:
                    if filter_search and filter_search not in path:
                        continue
                    if not params.show_hidden and path.startswith(".") and not path.startswith(".."):
                        continue
                    self.sortedfiltered.append((path, size, timestamp, uuid))
            use_sort = True
        entries.nbr_entries_filtered = len(self.sortedfiltered) + (1 if self.repo else 0)

        if use_sort:
            if self.repo:
                if params.sort_method == 'FILE_SORT_TIME':
                    self.sortedfiltered.sort(key=lambda e: e[1]["variants"][e[1]["variant_default"]]["revisions"][e[1]["variants"][e[1]["variant_default"]]["revision_default"]]["timestamp"])
                elif params.sort_method == 'FILE_SORT_SIZE':
                    self.sortedfiltered.sort(key=lambda e: e[1]["variants"][e[1]["variant_default"]]["revisions"][e[1]["variants"][e[1]["variant_default"]]["revision_default"]]["size"])
                elif params.sort_method == 'FILE_SORT_EXTENSION':
                    self.sortedfiltered.sort(key=lambda e: e[1]["blen_type"])
                else:
                    self.sortedfiltered.sort(key=lambda e: e[1]["name"].lower())
            else:
                if params.sort_method == 'FILE_SORT_TIME':
                    self.sortedfiltered.sort(key=lambda e: e[2])
                elif params.sort_method == 'FILE_SORT_SIZE':
                    self.sortedfiltered.sort(key=lambda e: e[1])
                else:
                    self.sortedfiltered.sort(key=lambda e: e[0].lower())
            return True
        return False

    def entries_block_get(self, start_index, end_index, entries):
#        print(entries.entries[:])
        if self.repo:
            if start_index == 0:
                entry = entries.entries.add()
                entry.type = {'DIR'}
                entry.relpath = '..'
                variant = entry.variants.add()
                entry.variants.active = variant
                rev = variant.revisions.add()
                variant.revisions.active = rev
            else:
                start_index -= 1
            end_index -= 1
            #~ print("self repo", len(self.sortedfiltered), start_index, end_index)
            for euuid, e in self.sortedfiltered[start_index:end_index]:
                self.entry_from_uuid(entries, euuid, (0, 0, 0, 0), (0, 0, 0, 0))
        else:
            #~ print("self dirs", len(self.sortedfiltered), start_index, end_index)
            for path, size, timestamp, uuid in self.sortedfiltered[start_index:end_index]:
                entry = entries.entries.add()
                entry.type = {'DIR'}
                entry.relpath = path
#                print("added entry for", entry.relpath)
                entry.uuid = uuid
                variant = entry.variants.add()
                entry.variants.active = variant
                rev = variant.revisions.add()
                rev.size = size
                rev.timestamp = timestamp
                variant.revisions.active = rev
        return True

    def entries_uuid_get(self, uuids, entries):
#        print(entries.entries[:])
        if self.repo:
            for uuid in uuids.uuids:
                self.entry_from_uuid(entries, tuple(uuid.uuid_asset), tuple(uuid.uuid_variant), tuple(uuid.uuid_revision))
            return True
        return False


##########
# UI stuff
class AMBERAIO_UL_tags_filter(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        # assert(isinstance(item, bpy.types.AmberTag))
        ae_amber = data
        tag = item
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            split = layout.split(0.66, False)
            split.prop(tag, "name", text="", emboss=False, icon_value=icon)
            row = split.row(align=True)
            sub = row.row(align=True)
            sub.active = tag.use_include
            sub.prop(tag, "use_include", emboss=False, text="", icon='ZOOMIN')
            sub = row.row(align=True)
            sub.active = tag.use_exclude
            sub.prop(tag, "use_exclude", emboss=False, text="", icon='ZOOMOUT')
        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            layout.label(text="", icon_value=icon)


class AmberPanel():
    @classmethod
    def poll(cls, context):
        space = context.space_data
        if space and space.type == 'FILE_BROWSER':
            ae = space.asset_engine
            if ae and space.asset_engine_type == "AssetEngineAmberAIO":
                return True
        return False


class AMBERAIO_PT_options(Panel, AmberPanel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_category = "Asset Engine"
    bl_label = "AmberAIO Options"

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        ae = space.asset_engine

        row = layout.row()


class AMBERAIO_PT_tags(Panel, AmberPanel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_category = "Filter"
    bl_label = "Tags"

    def draw(self, context):
        ae = context.space_data.asset_engine

        # Note: This is *ultra-primitive*!
        #       A good UI will most likely need new widget option anyway (template). Or maybe just some UIList...
        #~ self.layout.props_enum(ae, "tags")
        self.layout.template_list("AMBERAIO_UL_tags_filter", "", ae, "tags", ae, "active_tag_index")


if __name__ == "__main__":  # only for live edit.
    bpy.utils.register_module(__name__)
