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

# Blender Cloud Asset Engine, using Pillar and based on the Cloud addon.
# "Claude" (French first name pronounced in a similar way as “cloud”) stands for Cloud Light Asset Under Development Engine.
# Note: This will be a simple addon later, but until it gets to master, it's simpler to have it as a startup module!

import bpy
from bpy.types import (
        AssetEngine,
        Panel,
        PropertyGroup,
        )
from bpy.props import (
        StringProperty,
        BoolProperty,
        IntProperty,
        FloatProperty,
        EnumProperty,
        CollectionProperty,
        )

# Support reloading
if 'pillar' in locals():
    import importlib

    wheels = importlib.reload(wheels)
    wheels.load_wheels()

    pillar = importlib.reload(pillar)
    cache = importlib.reload(cache)
else:
    from . import wheels

    wheels.load_wheels()

    from . import pillar, cache

import concurrent.futures as futures
import asyncio
import threading

import binascii
import hashlib
import json
import os
import pathlib
import stat
import struct
import time
import random

from collections import OrderedDict

import pillarsdk

REQUIRED_ROLES_FOR_CLAUDE = {'subscriber', 'demo'}


##########
# Helpers.
class SpecialFolderNode(pillarsdk.Node):
    pass


class UpNode(SpecialFolderNode):
    def __init__(self):
        super().__init__()
        self['_id'] = 'UP'
        self['node_type'] = 'UP'
        self['name'] = ".."


class ProjectNode(SpecialFolderNode):
    def __init__(self, project):
        super().__init__()

        assert isinstance(project, pillarsdk.Project), "wrong type for project: %r" % type(project)

        self.merge(project.to_dict())
        self['node_type'] = 'PROJECT'


class ClaudeRepository:
    def __init__(self):
        self.path_map = {}  # Mapping names->uuid of Cloud nodes (so that user gets friendly 'named' paths in browser).
        self.curr_path = pillar.CloudPath("/")  # Using node names.
        self.curr_path_real = pillar.CloudPath("/")  # Using node uuid's.
        self.pending_path = []  # Defined when we get a path from browser (e.g. user input) that we don't know of...

        self.nodes = OrderedDict()  # uuid's to nodes mapping, for currently browsed area...
        self.is_ready = False

    def check_dir_do(self, root_path, do_change, do_update_intern):
        if not do_update_intern and self.curr_path == root_path:
            return True, root_path
        parts = root_path.parts
        if do_update_intern:
            self.pending_path = []
            self.is_ready = False
            self.nodes.clear()
        if parts and parts[0] == "/":
            nids = []
            paths = self.path_map
            idx = 0
            for i, p in enumerate(parts[1:]):
                idx = i + 2
                nid, paths = paths.get(p, (None, None))
                #~ print(p, nid, paths)
                if nid is None:
                    idx -= 1
                    break
                else:
                    nids.append(nid)
            if do_update_intern:
                self.pending_path = parts[idx:]
                self.curr_path = pillar.CloudPath("/" + "/".join(parts[1:idx]))
                self.curr_path_real = pillar.CloudPath("/" + "/".join(nids))
            return True, root_path
        elif do_change:
            if do_update_intern:
                self.curr_path = pillar.CloudPath("/")
                self.curr_path_real = pillar.CloudPath("/")
            return True, pillar.CloudPath("/")
        else:
            return False, root_path


#############
# Claude Jobs.
class ClaudeJob:
    @staticmethod
    def async_looper(func):
        def wrapper(self, *args, **kwargs):
            loop = self.loop
            assert(not loop.is_running())
            print("proceed....")
            ret = func(self, *args, **kwargs)
            if not self.evt_cancel.is_set():
                print("kickstep")
                # This forces loop to only do 'one step', and return (hopefully!) very soon.
                #~ loop.stop()
                loop.call_soon(loop.stop)
                loop.run_forever()
            else:
                print("canceled")
            return ret
        return wrapper

    def add_task(self, coro):
        task = asyncio.ensure_future(coro)
        self._tasks.append(task)
        return task

    def remove_task(self, task):
        self._tasks.remove(task)
        return None

    def cancel(self):
        print("Cancelling ", self)
        self.evt_cancel.set()
        for task in self._tasks:
            task.cancel()
        self._tasks[:] = []
        self.status = {'VALID'}
        assert(not self.loop.is_running())
        self.loop.call_soon(self.loop.stop)
        self.loop.run_forever()

    def __init__(self, job_id):
        self.job_id = job_id
        self.status = {'VALID'}
        self.progress = 0.0

        self._tasks = []

        self.evt_cancel = threading.Event()
        self.loop = asyncio.get_event_loop()

    def __del__(self):
        print("deleting ", self)
        self.cancel()

class ClaudeJobCheckCredentials(ClaudeJob):
    @staticmethod
    async def check():
        """
        Check credentials with Pillar, and if ok returns the user ID.
        Returns None if the user cannot be found, or if the user is not a Cloud subscriber.
        """
        try:
            user_id = await pillar.check_pillar_credentials(REQUIRED_ROLES_FOR_CLAUDE)
        except pillar.NotSubscribedToCloudError:
            print('Not subscribed.')
            return None
        except pillar.CredentialsNotSyncedError:
            print('Credentials not synced, re-syncing automatically.')
            #~ self.log.info('Credentials not synced, re-syncing automatically.')
        else:
            #~ self.log.info('Credentials okay.')
            return user_id

        try:
            user_id = await pillar.refresh_pillar_credentials(required_roles)
        except pillar.NotSubscribedToCloudError:
            print('Not subscribed.')
            return None
        except pillar.UserNotLoggedInError:
            print('User not logged in on Blender ID.')
            #~ self.log.error('User not logged in on Blender ID.')
        else:
            #~ self.log.info('Credentials refreshed and ok.')
            return user_id

        return None

    @ClaudeJob.async_looper
    def update(self):
        self.status = {'VALID', 'RUNNING'}
        user_id = ...

        if self.check_task is ...:
            self.check_task = self.add_task(self.check())

        self.progress += 0.01
        if (self.progress > 1.0):
            self.progress = 0.0

        if self.check_task is not None:
            if not self.check_task.done():
                return ...
            user_id = self.check_task.result()
            self.check_task = self.remove_task(self.check_task)
            self.progress = 1.0

        if self.check_task is None:
            self.progress = 1.0
            self.status = {'VALID'}
        return user_id

    def __init__(self, job_id):
        super().__init__(job_id)

        self.check_task = ...

        self.update()


class ClaudeJobList(ClaudeJob):
    @staticmethod
    async def ls(repo):
        print("we should be listing Cloud content from %s..." % repo.curr_path_real)

        async def ls_do(part):
            project_uuid = repo.curr_path_real.project_uuid
            node_uuid = repo.curr_path_real.node_uuid

            # XXX Not nice to redo whole path, should be cached...
            curr_path_map = repo.path_map
            for pname, puuid in zip(repo.curr_path.parts[1:], repo.curr_path_real.parts[1:]):
                nid, curr_path_map = curr_path_map[pname]
                assert(nid == puuid)
            curr_path_map.clear()

            if node_uuid:
                # Query for sub-nodes of this node.
                print("Getting subnodes for parent node %r (%s)" % (node_uuid, repo.curr_path))
                children = [UpNode()]
                children += await pillar.get_nodes(parent_node_uuid=node_uuid, node_type=('group_texture', 'texture'))
            elif project_uuid:
                # Query for top-level nodes.
                print("Getting subnodes for project node %r" % project_uuid)
                children = [UpNode()]
                children += await pillar.get_nodes(project_uuid=project_uuid, parent_node_uuid='', node_type=('group_texture', 'texture'))
            else:
                # Query for projects
                print("No node UUID and no project UUID, listing available projects")
                children = await pillar.get_texture_projects()
                children = [ProjectNode(proj_dict) for proj_dict in children]

            for node in children:
                repo.nodes[node['_id']] = node
                curr_path_map[node['name']] = (node['_id'], {})

            if part is None:
                return True

            if part in curr_path_map:
                repo.curr_path /= part
                repo.curr_path_real /= curr_path_map[part][0]
                return True
            return False

        for ppath in repo.pending_path:
            if not await ls_do(ppath):
                break
        await ls_do(None)

        repo.pending_path = []
        repo.is_ready = True

    @ClaudeJob.async_looper
    def update(self, user_id):
        self.status = {'VALID', 'RUNNING'}

        if user_id is None:
            self.cancel()
            return

        self.progress += 0.01
        if (self.progress > 1.0):
            self.progress = 0.0

        if user_id is ...:
            return

        if self.ls_task is ...:  # INIT
            self.ls_task = self.add_task(self.ls(self.repo))

        if self.ls_task not in {None, ...}:
            if self.ls_task.done():
                print("ls finished, we should have our children nodes now!\n\n\n")
                self.ls_task = self.remove_task(self.ls_task)

        if self.ls_task is None:
            self.progress = 1.0
            self.status = {'VALID'}

    def __init__(self, job_id, user_id, repo):
        super().__init__(job_id)
        self.repo = repo

        self.ls_task = ...

        self.update(user_id)


class ClaudeJobPreviews(ClaudeJob):
    @staticmethod
    async def preview(node, thumb_dir):
        thumb_path = ""
        thumb_path_ready = asyncio.Event()

        def thumbnail_loading(node, texture_node):
            print("LOADING ", texture_node['name'], node['name'])

        def thumbnail_loaded(node, file_desc, thmb_p):
            nonlocal thumb_path
            thumb_path = thmb_p
            print("LOADED in ", thumb_path)
            thumb_path_ready.set()

        print("awaiting thumb for %s" % node["name"])
        await pillar.download_texture_thumbnail(node, 's', thumb_dir,
                                                thumbnail_loading=thumbnail_loading,
                                                thumbnail_loaded=thumbnail_loaded)
        await thumb_path_ready.wait()
        print("DDDDOOOOONNNNNNEEEEEE", thumb_path)

        if thumb_path:
            print(thumb_path)
            from PIL import Image
            thumb = Image.open(thumb_path)
            pixels = tuple(c for p in thumb.getdata() for c in p)
            return [thumb.size, pixels]

    @ClaudeJob.async_looper
    def update(self, user_id, uuids):
        self.status = {'VALID', 'RUNNING'}

        if user_id is None:
            self.cancel()
            return

        if user_id is ...:
            self.progress += 0.01
            if (self.progress > 1.0):
                self.progress = 0.0
            return

        uuids = {tuple(uuid.uuid_asset): uuid for uuid in uuids.uuids}
        new_uuids = set(uuids)
        old_uuids = set(self.prv_tasks)
        del_uuids = old_uuids - new_uuids
        new_uuids -= old_uuids

        print("\n\n\n")
        print(new_uuids)
        print(del_uuids)
        print(old_uuids)
        print("\n\n\n")

        for uuid in del_uuids:
            self.prv_tasks[uuid].cancel()
            self.remove_task(self.prv_tasks[uuid])
            del self.prv_tasks[uuid]

        for uuid in new_uuids:
            node = tuple(self.repo.nodes.values())[uuid[-1]]
            if node['node_type'] != 'texture':
                uuids[uuid].has_asset_preview = False
                continue
            self.prv_tasks[uuid] = self.add_task(self.preview(node, self.thumb_dir))

        tot = len(self.prv_tasks)
        nbr_done = 0

        done_uuids = set()
        for uuid, tsk in self.prv_tasks.items():
            if tsk.done():
                size, pixels = tsk.result()
                print(size, pixels[:10])
                uuids[uuid].preview_size = size
                uuids[uuid].preview_pixels = pixels
                nbr_done += 1
                done_uuids.add(uuid)

        for uuid in done_uuids:
            self.remove_task(self.prv_tasks[uuid])
            del self.prv_tasks[uuid]

        self.progress = nbr_done / tot if tot else 1.0
        if not self.prv_tasks:
            self.status = {'VALID'}

    def __init__(self, job_id, user_id, repo, uuids, thumb_dir):
        super().__init__(job_id)
        self.repo = repo
        self.thumb_dir = thumb_dir

        self.prv_tasks = {}

        self.update(user_id, uuids)


###########################
# Main Asset Engine class.
class AssetEngineClaude(AssetEngine):
    bl_label = "Claude"
    bl_version = (0 << 16) + (0 << 8) + 1  # Usual maj.min.rev version scheme...

    def __init__(self):
        self.jobs = {}

        self.job_uuid = 1
        self.user_id = ...
        self.job_id_check_credentials = ...
        self.check_credentials()

        self.repo = ClaudeRepository()

    def __del__(self):
        pass

    ########## Various helpers ##########
    def check_credentials(self):
        if self.job_id_check_credentials is None:
            return self.user_id
        if self.job_id_check_credentials is ...:
            self.job_id_check_credentials = job_id = self.job_uuid
            self.job_uuid += 1
            job = self.jobs[job_id] = ClaudeJobCheckCredentials(job_id)
        else:
            job = self.jobs[self.job_id_check_credentials]
        if job is not None:
            self.user_id = job.update()
            if self.user_id is not ...:
                self.kill(self.job_id_check_credentials)
                self.job_id_check_credentials = None
        return self.user_id

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
        return (progress / nbr_jobs) if nbr_jobs else 0.0

    def kill(self, job_id):
        if job_id:
            self.jobs.pop(job_id, None)
            return
        self.jobs.clear()

    def list_dir(self, job_id, entries):
        user_id = self.check_credentials()

        curr_path = pillar.CloudPath(entries.root_path)
        if curr_path != self.repo.curr_path:
            print(curr_path, self.repo.curr_path)
            self.repo.check_dir_do(curr_path, True, True)
            print(curr_path, self.repo.curr_path)

        if not self.repo.is_ready:
            curr_path = self.repo.curr_path_real
            pending_path = self.repo.pending_path
            print("listing %s%s[%s]..." % (str(curr_path), "" if str(curr_path).endswith('/') else "/", "/".join(pending_path)))

            job = self.jobs.get(job_id, None)
            if job is None or not isinstance(job, ClaudeJobList):
                job_id = self.job_uuid
                self.job_uuid += 1
                job = self.jobs[job_id] = ClaudeJobList(job_id, user_id, self.repo)
            if job is not None:
                job.update(user_id)
                print(job.status)
                if self.repo.is_ready or 'RUNNING' not in job.status:
                    entries.root_path = str(self.repo.curr_path)
                    self.kill(job_id)

        #~ print(list(self.repo.nodes.values()))
        entries.nbr_entries = len(self.repo.nodes)
        return job_id

    def sort_filter(self, use_sort, use_filter, params, entries):
        entries.nbr_entries_filtered = len(self.repo.nodes)
        return False

    def update_check(self, job_id, uuids):
        # do nothing for now, no need to use actual job...
        return self.job_id_invalid

    # TODO
    """
    def ensure_uuids(self, job_id, uuids):
        from pillarsdk.utils import sanitize_filename

        nodes = list(self.repo.nodes.values())

        for uuid in uuids.uuids:
            node = uuid.asset_uuid[-1]
            node_path_components = (self.repo.curr_path / node['name']).parts[1:]  # Ignore root '/' here...
            local_path_components = [sanitize_filename(comp) for comp in node_path_components]

            top_texture_directory = bpy.app.tempdir  # bpy.path.abspath(context.scene.local_texture_dir)
            local_path = os.path.join(top_texture_directory, *local_path_components)
            meta_path = os.path.join(top_texture_directory, '.blender_cloud')

            printf("we should be downloading %s into %s..." % (node_path_components, local_path))

        return self.job_id_invalid
    """

    def load_pre(self, uuids, entries):
        from pillarsdk.utils import sanitize_filename

        nodes = list(self.repo.nodes.values())

        # Note that most of this should ideally happen in ensure_uuids... later...
        for uuid in uuids.uuids:
            euuid = tuple(uuid.uuid_asset)
            vuuid = tuple(uuid.uuid_variant)
            ruuid = tuple(uuid.uuid_revision)

            node = nodes[euuid[-1]]
            print(self.repo.curr_path / node['name'])
            node_path_components = (self.repo.curr_path / node['name']).parts[1:]  # Ignore root '/' here...
            local_path_components = [sanitize_filename(comp) for comp in node_path_components]

            top_texture_directory = bpy.app.tempdir  # bpy.path.abspath(context.scene.local_texture_dir)
            local_path = os.path.join(top_texture_directory, *local_path_components)
            meta_path = os.path.join(top_texture_directory, '.blender_cloud')

            print("we should be downloading %s into %s..." % (node_path_components, local_path))

            def texture_downloaded(file_path, file_desc, *args):
                entry = entries.entries.add()
                entry.type = {'BLENLIB'}
                entry.blender_type = 'IMAGE'
                entry.relpath = file_path
                entry.uuid = euuid
                var = entry.variants.add()
                var.uuid = vuuid
                rev = var.revisions.add()
                rev.uuid = ruuid
                var.revisions.active = rev
                entry.variants.active = var

            def texture_downloading(file_path, file_desc, *args):
                pass

            signalling_future = asyncio.Future()
            asyncio.get_event_loop().run_until_complete(pillar.download_texture(node, local_path,
                                                                                metadata_directory=meta_path,
                                                                                texture_loading=texture_downloading,
                                                                                texture_loaded=texture_downloaded,
                                                                                future=signalling_future))

        entries.root_path = ""
        return True

    def previews_get(self, job_id, uuids):
        user_id = self.check_credentials()

        job = self.jobs.get(job_id, None)
        if job is not None and isinstance(job, ClaudeJobPreviews):
            job.update(user_id, uuids)
        else:
            top_texture_directory = bpy.app.tempdir  # bpy.path.abspath(context.scene.local_texture_dir)
            thumb_dir = os.path.join(top_texture_directory, "__thumbs")

            job_id = self.job_uuid
            self.job_uuid += 1
            self.jobs[job_id] = ClaudeJobPreviews(job_id, user_id, self.repo, uuids, thumb_dir)
        return job_id

    def check_dir(self, entries, do_change):
        #~ print(self.repo.path_map)
        ret, root_path = self.repo.check_dir_do(pillar.CloudPath(entries.root_path), do_change, False)
        entries.root_path = str(root_path)
        return ret

    def entries_block_get(self, start_index, end_index, entries):
        #~ print(start_index, end_index)
        #~ print(self.repo.nodes)
        #~ print("\n\n\n")
        for num, (uuid, node) in enumerate(tuple(self.repo.nodes.items())[start_index:end_index]):
            entry = entries.entries.add()
            entry.type = {'IMAGE'} if node['node_type'] == 'texture' else {'DIR'}
            entry.relpath = node['name']
            #~ print("added entry for", entry.relpath)
            entry.uuid = (0, 0, 0, num + start_index)
            variant = entry.variants.add()
            entry.variants.active = variant
            rev = variant.revisions.add()
            variant.revisions.active = rev
        return True

    def entries_uuid_get(self, uuids, entries):
        return False


##########
# UI stuff

class ClaudePanel():
    @classmethod
    def poll(cls, context):
        space = context.space_data
        if space and space.type == 'FILE_BROWSER':
            ae = space.asset_engine
            if ae and space.asset_engine_type == "AssetEngineClaude":
                return True
        return False


class CLAUDE_PT_messages(Panel, ClaudePanel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_category = "Asset Engine"
    bl_label = "Claude Messages"

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        ae = space.asset_engine

        layout.label("Some stupid test info...", icon='INFO')


if __name__ == "__main__":  # only for live edit.
    bpy.utils.register_module(__name__)
